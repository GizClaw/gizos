#include "h2_bk7258_board_private.h"

#include <string.h>

#ifdef H2_H202_HOST_TEST
#include "h2_h202_board_test_sdk.h"
#else
#include "driver/gpio.h"
#include "gpio_driver.h"
#include "driver/spi.h"
#include "os/os.h"
#endif

/* H106's WS2812 lamp board: MSB-first GRB, each bit encoded as 1000/1110.
 * At 3.2 MHz the high times are 312.5/937.5 ns. Reset low is 160 us on
 * each side of the complete six-pixel frame. No per-pixel transactions. */
enum { RESET_BYTES = 64, ENCODED_BYTES = H2_H202_RGB_PIXELS * 12 };
static uint8_t s_tx[RESET_BYTES * 2 + ENCODED_BYTES];
static h2_pal_led_color_t s_pixels[H2_H202_RGB_PIXELS];
static beken_semaphore_t s_tx_done;
static bool s_spi_live;
static bool s_semaphore_live;
static bool s_spi_ready;
static unsigned s_brightness = 35;
static bool s_white_on;

static void tx_complete(spi_id_t id, void* user) {
  (void)id;
  (void)user;
  /* The SDK's semaphore implementation dispatches its ISR-safe give. */
  (void)rtos_set_semaphore(&s_tx_done);
}

static h2_pal_result_t init_rgb(void) {
  if (s_spi_ready) return H2_PAL_OK;
  if (s_spi_live && bk_spi_deinit(H2_H202_RGB_SPI_ID) != 0)
    return H2_PAL_ERR_IO;
  s_spi_live = false;
  if (!s_semaphore_live) {
    if (rtos_init_semaphore(&s_tx_done, 1) != 0) return H2_PAL_ERR_NO_MEMORY;
    s_semaphore_live = true;
  }
  if (bk_spi_driver_init() != 0) return H2_PAL_ERR_IO;
  const spi_config_t config = {
      .role = SPI_ROLE_MASTER,
      .bit_width = SPI_BIT_WIDTH_8BITS,
      .polarity = SPI_POLARITY_LOW,
      .phase = SPI_PHASE_1ST_EDGE,
      .wire_mode = SPI_4WIRE_MODE,
      .baud_rate = H2_H202_RGB_SPI_HZ,
      .bit_order = SPI_MSB_FIRST,
      .dma_mode = SPI_DMA_MODE_DISABLE,
  };
  const gpio_id_t preserve[] = {H2_H202_POWER_KEY_GPIO, H2_H202_USB_INSERT_GPIO,
                                5};
  uint32_t values[3];
  for (unsigned i = 0; i < 3; ++i) values[i] = bk_gpio_get_value(preserve[i]);
  int rc = bk_spi_init(H2_H202_RGB_SPI_ID, &config);
  /* Init can partially map pins even on error. Reclaim CLK/CS/MISO in either
   * case: Power, insertion sensing and modem RI are not SPI pins on H202. */
  s_spi_live = true;
  for (unsigned i = 0; i < 3; ++i) {
    int unmap_rc = gpio_dev_unmap(preserve[i]);
    int restore_rc = bk_gpio_set_value(preserve[i], values[i]);
    if (unmap_rc != 0 || restore_rc != 0) rc = -1;
  }
  if (rc != 0 ||
      bk_spi_register_tx_finish_isr(H2_H202_RGB_SPI_ID, tx_complete, NULL) != 0)
    return H2_PAL_ERR_IO;
  s_spi_ready = true;
  return H2_PAL_OK;
}

static void encode(uint8_t value, uint8_t* out) {
  for (unsigned i = 0; i < 4; ++i) {
    unsigned shift = 6u - 2u * i;
    unsigned pair = (value >> shift) & 3u;
    out[i] = (uint8_t)(((pair & 2u) ? 0xe0u : 0x80u) |
                       ((pair & 1u) ? 0x0eu : 0x08u));
  }
}

static h2_pal_result_t transmit(void) {
  h2_pal_result_t rc = init_rgb();
  if (rc != H2_PAL_OK) return rc;
  memset(s_tx, 0, sizeof(s_tx));
  for (unsigned i = 0; i < H2_H202_RGB_PIXELS; ++i) {
    uint8_t* output = s_tx + RESET_BYTES + i * 12u;
    encode((uint8_t)(s_pixels[i].g * s_brightness / 100u), output);
    encode((uint8_t)(s_pixels[i].r * s_brightness / 100u), output + 4);
    encode((uint8_t)(s_pixels[i].b * s_brightness / 100u), output + 8);
  }
  (void)rtos_get_semaphore(&s_tx_done, 0);
  if (bk_spi_write_bytes_async(H2_H202_RGB_SPI_ID, s_tx, sizeof(s_tx)) == 0 &&
      rtos_get_semaphore(&s_tx_done, 20) == 0)
    return H2_PAL_OK;
  /* Retain static storage and semaphore if deinit fails; no next write can
   * reuse the buffer while hardware/ISR ownership remains unresolved. */
  s_spi_ready = false;
  if (bk_spi_deinit(H2_H202_RGB_SPI_ID) == 0) s_spi_live = false;
  return H2_PAL_ERR_IO;
}

static h2_pal_result_t info(void* user, h2_pal_led_id_t id,
                            h2_pal_led_info_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (id != H2_H202_EYES_ID) return H2_PAL_ERR_NOT_FOUND;
  *out = (h2_pal_led_info_t){.id = id,
                             .pixel_count = H2_H202_RGB_PIXELS,
                             .channels_per_pixel = 3,
                             .supports_brightness = 1};
  return H2_PAL_OK;
}

static h2_pal_result_t frame(void* user, h2_pal_led_id_t id,
                             const h2_pal_led_color_t* pixels, size_t count) {
  (void)user;
  if (id != H2_H202_EYES_ID) return H2_PAL_ERR_NOT_FOUND;
  if (pixels == NULL || count != H2_H202_RGB_PIXELS)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0; i < count; ++i)
    if (pixels[i].w != 0) return H2_PAL_ERR_UNSUPPORTED;
  memcpy(s_pixels, pixels, sizeof(s_pixels));
  return transmit();
}

static h2_pal_result_t solid(void* user, h2_pal_led_id_t id,
                             h2_pal_led_color_t color) {
  h2_pal_led_color_t pixels[H2_H202_RGB_PIXELS];
  for (unsigned i = 0; i < H2_H202_RGB_PIXELS; ++i) pixels[i] = color;
  return frame(user, id, pixels, H2_H202_RGB_PIXELS);
}

static h2_pal_result_t clear(void* user, h2_pal_led_id_t id) {
  return solid(user, id, (h2_pal_led_color_t){0});
}

static h2_pal_result_t brightness(void* user, h2_pal_led_id_t id,
                                  uint8_t percent) {
  (void)user;
  if (id != H2_H202_EYES_ID) return H2_PAL_ERR_NOT_FOUND;
  if (percent > 100) return H2_PAL_ERR_INVALID_ARG;
  s_brightness = percent;
  return transmit();
}

/* Board init configures GPIO37 as a low output; this only changes its level. */
h2_pal_result_t h2_h202_white_set(bool on) {
  gpio_id_t gpio = (gpio_id_t)H2_H202_WHITE_LED_GPIO;
  if ((on ? bk_gpio_set_output_high(gpio) : bk_gpio_set_output_low(gpio)) != 0)
    return H2_PAL_ERR_IO;
  s_white_on = on;
  return H2_PAL_OK;
}

bool h2_h202_white_on(void) { return s_white_on; }

const h2_pal_led_api_t* h2_h202_led_api(void) {
  static const h2_pal_led_vtable_t vtable = {
      .get_info = info,
      .set_frame = frame,
      .set_solid = solid,
      .clear = clear,
      .set_brightness_percent = brightness};
  static const h2_pal_led_api_t api = {.vtable = &vtable};
  return &api;
}

h2_pal_result_t h2_h202_led_deinit(void) {
  if (s_white_on && h2_h202_white_set(false) != H2_PAL_OK) return H2_PAL_ERR_IO;
  if (s_spi_live) {
    if (bk_spi_deinit(H2_H202_RGB_SPI_ID) != 0) return H2_PAL_ERR_IO;
    s_spi_live = s_spi_ready = false;
  }
  if (s_semaphore_live) {
    if (rtos_deinit_semaphore(&s_tx_done) != 0) return H2_PAL_ERR_IO;
    s_semaphore_live = false;
  }
  return H2_PAL_OK;
}
