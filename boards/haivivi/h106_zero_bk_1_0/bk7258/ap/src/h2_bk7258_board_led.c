#include "h2_bk7258_board_private.h"
#include "h2_zero_bk_1_0_board_config.h"
#include "h2_zero_bk_spi.h"

#include <common/bk_err.h>
#include <driver/dma_types.h>
#include <driver/gpio.h>
#include <driver/spi.h>
#include "gpio_driver.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define H2_ZERO_BK_WS2812_LEAD_BYTES 64u
#define H2_ZERO_BK_WS2812_TRAIL_BYTES 64u
#define H2_ZERO_BK_WS2812_BYTES_PER_PIXEL 12u
#define H2_ZERO_BK_WS2812_TX_SIZE                                             \
  (H2_ZERO_BK_WS2812_LEAD_BYTES +                                             \
   H2_ZERO_BK_1_0_LED_STRIP_PIXEL_COUNT * H2_ZERO_BK_WS2812_BYTES_PER_PIXEL + \
   H2_ZERO_BK_WS2812_TRAIL_BYTES)

static const uint8_t s_ws2812_encode[4] = {0x88u, 0x8eu, 0xe8u, 0xeeu};
static uint8_t s_tx[H2_ZERO_BK_WS2812_TX_SIZE] __attribute__((aligned(8)));
static uint8_t s_brightness_percent = 100u;
static int s_spi_initialized;

static h2_pal_result_t validate_led_id(h2_pal_led_id_t id) {
  if (id == H2_ZERO_BK_1_0_LED_STRIP_ID) {
    return H2_PAL_OK;
  }
  h2_pal_periph_info_t info;
  h2_pal_result_t rc =
      h2_pal_periph_get(h2_bk7258_board_periph_api(), id, &info);
  return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_ARG : rc;
}

static h2_pal_result_t ensure_spi(void) {
  if (s_spi_initialized != 0) {
    return H2_PAL_OK;
  }
#if defined(CONFIG_SPI_DMA) && CONFIG_SPI_DMA
  const spi_config_t config = {
      .role = SPI_ROLE_MASTER,
      .bit_width = SPI_BIT_WIDTH_8BITS,
      .polarity = SPI_POLARITY_LOW,
      .phase = SPI_PHASE_1ST_EDGE,
      .wire_mode = SPI_4WIRE_MODE,
      .baud_rate = H2_ZERO_BK_1_0_LED_STRIP_FREQUENCY_HZ,
      .bit_order = SPI_MSB_FIRST,
      .dma_mode = SPI_DMA_MODE_ENABLE,
      .spi_tx_dma_width = DMA_DATA_WIDTH_8BITS,
      .spi_rx_dma_width = DMA_DATA_WIDTH_8BITS,
  };
  if (bk_spi_driver_init() != BK_OK) {
    return H2_PAL_ERR_IO;
  }
  /* BK7258 SPI1 init maps all four bus pins. WS2812 uses only MOSI;
   * preserve Power, USB detect and audio select, including output state.
   * Restore even when SPI initialization fails. */
  const gpio_id_t preserved_pins[] = {
      (gpio_id_t)H2_ZERO_BK_1_0_POWER_BUTTON_GPIO,
      (gpio_id_t)H2_ZERO_BK_1_0_USB_INSERT_GPIO,
      (gpio_id_t)H2_ZERO_BK_1_0_AUDIO_SELECT_GPIO,
  };
  uint32_t saved_config[3];
  for (size_t i = 0u; i < 3u; ++i) {
    saved_config[i] = bk_gpio_get_value(preserved_pins[i]);
  }
  bk_err_t rc = bk_spi_init((spi_id_t)H2_ZERO_BK_1_0_LED_STRIP_SPI_ID, &config);
  for (size_t i = 0u; i < 3u; ++i) {
    const bk_err_t unmap_rc = gpio_dev_unmap(preserved_pins[i]);
    const bk_err_t restore_rc =
        bk_gpio_set_value(preserved_pins[i], saved_config[i]);
    printf(
        "H2_ZERO_BK_SPI_PIN gpio=%u before=%08lx after=%08lx unmap=%d "
        "restore=%d\n",
        (unsigned)preserved_pins[i], (unsigned long)saved_config[i],
        (unsigned long)bk_gpio_get_value(preserved_pins[i]), (int)unmap_rc,
        (int)restore_rc);
    if (unmap_rc != BK_OK || restore_rc != BK_OK) {
      rc = BK_FAIL;
    }
  }
  if (rc != BK_OK) {
    return H2_PAL_ERR_IO;
  }
  s_spi_initialized = 1;
  return H2_PAL_OK;
#else
  return H2_PAL_ERR_UNSUPPORTED;
#endif
}

static uint8_t scale_channel(uint8_t value) {
  return (uint8_t)(((uint16_t)value * s_brightness_percent) / 100u);
}

static void encode_byte(uint8_t value, uint8_t* out) {
  out[0] = s_ws2812_encode[(value >> 6) & 3u];
  out[1] = s_ws2812_encode[(value >> 4) & 3u];
  out[2] = s_ws2812_encode[(value >> 2) & 3u];
  out[3] = s_ws2812_encode[value & 3u];
}

static h2_pal_result_t transmit_frame(const h2_pal_led_color_t* pixels) {
  h2_pal_result_t rc = ensure_spi();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  memset(s_tx, 0, sizeof(s_tx));
  uint8_t* out = s_tx + H2_ZERO_BK_WS2812_LEAD_BYTES;
  for (size_t i = 0u; i < H2_ZERO_BK_1_0_LED_STRIP_PIXEL_COUNT; ++i) {
    encode_byte(scale_channel(pixels[i].g), out);
    encode_byte(scale_channel(pixels[i].r), out + 4u);
    encode_byte(scale_channel(pixels[i].b), out + 8u);
    out += H2_ZERO_BK_WS2812_BYTES_PER_PIXEL;
  }
  return h2_zero_bk_spi_dma_write((spi_id_t)H2_ZERO_BK_1_0_LED_STRIP_SPI_ID,
                                  s_tx, sizeof(s_tx)) == BK_OK
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}

static h2_pal_result_t led_get_info(void* user, h2_pal_led_id_t id,
                                    h2_pal_led_info_t* out_info) {
  (void)user;
  if (out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = validate_led_id(id);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  *out_info = (h2_pal_led_info_t){
      .id = id,
      .pixel_count = H2_ZERO_BK_1_0_LED_STRIP_PIXEL_COUNT,
      .channels_per_pixel = 3u,
      .supports_brightness = 1u,
      .frame_commit_is_atomic = 1u,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t led_set_frame(void* user, h2_pal_led_id_t id,
                                     const h2_pal_led_color_t* pixels,
                                     size_t pixel_count) {
  (void)user;
  h2_pal_result_t rc = validate_led_id(id);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (pixels == NULL || pixel_count != H2_ZERO_BK_1_0_LED_STRIP_PIXEL_COUNT) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return transmit_frame(pixels);
}

static h2_pal_result_t led_set_solid(void* user, h2_pal_led_id_t id,
                                     h2_pal_led_color_t color) {
  (void)user;
  h2_pal_result_t rc = validate_led_id(id);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  h2_pal_led_color_t pixels[H2_ZERO_BK_1_0_LED_STRIP_PIXEL_COUNT];
  for (size_t i = 0u; i < H2_ZERO_BK_1_0_LED_STRIP_PIXEL_COUNT; ++i) {
    pixels[i] = color;
  }
  return transmit_frame(pixels);
}

static h2_pal_result_t led_clear(void* user, h2_pal_led_id_t id) {
  return led_set_solid(
      user, id, (h2_pal_led_color_t){.r = 0u, .g = 0u, .b = 0u, .w = 0u});
}

static h2_pal_result_t led_set_brightness_percent(void* user,
                                                  h2_pal_led_id_t id,
                                                  uint8_t percent) {
  (void)user;
  h2_pal_result_t rc = validate_led_id(id);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (percent > 100u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  s_brightness_percent = percent;
  return H2_PAL_OK;
}

const h2_pal_led_api_t* h2_bk7258_board_led_api(void) {
  static const h2_pal_led_vtable_t vtable = {
      .get_info = led_get_info,
      .set_frame = led_set_frame,
      .set_solid = led_set_solid,
      .clear = led_clear,
      .set_brightness_percent = led_set_brightness_percent,
  };
  static const h2_pal_led_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}
