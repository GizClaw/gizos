#include "asm/includes.h"
#include "asm/adc_api.h"
#include "device/device.h"
#include "device/iic.h"
#include "device/ioctl_cmds.h"
#include "gpio.h"
#include "os/os_api.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_wl82_atomic.h"

#include <stddef.h>
#include <stdint.h>

enum {
  H2_LCD_WIDTH = 480,
  H2_LCD_HEIGHT = 320,
  H2_LCD_RESET_PIN = IO_PORTB_00,
  H2_LCD_RS_PIN = IO_PORTC_09,
  H2_LCD_BACKLIGHT_PIN = IO_PORTB_08,
  H2_LCD_CHECK_D6_PIN = IO_PORTC_07,
  H2_LCD_CHECK_D7_PIN = IO_PORTC_08,
  H2_TOUCH_INT_PIN = IO_PORTB_04,
  H2_ADKEY_PIN = IO_PORTB_01,
  H2_FT6236_WRITE = 0x70,
  H2_FT6236_READ = 0x71,
  H2_FT6236_FINGERS = 0x02,
  H2_FT6236_POINT1 = 0x03,
  H2_FT6236_CHIP_ID = 0xa3,
};

typedef struct h2_panel_init {
  uint8_t command;
  uint8_t count;
  uint8_t data[12];
} h2_panel_init_t;

typedef struct h2_display_state {
  void *device;
  /* The EMI driver keeps the source buffer until its non-blocking transfer
   * completes. Keep a board-owned full-frame staging buffer, wait before
   * reusing it, and submit one transfer per frame like JieLi's LCD driver. */
  uint8_t frame[H2_LCD_WIDTH * H2_LCD_HEIGHT * 2u];
  int open;
  int pending;
  uint32_t dma_done;
} h2_display_state_t;

typedef struct h2_touch_state {
  void *iic;
  int open;
  int down;
  uint16_t x;
  uint16_t y;
} h2_touch_state_t;

static h2_display_state_t display_state;
static h2_touch_state_t touch_state;
static uint32_t adkey_initialized;
static uint32_t display_operation;
static uint32_t touch_operation;

/* Nonwaiting operation reservations serialize each singleton device. SDK
 * calls run without a held PAL mutex/spinlock; callback reentry returns BUSY. */
#define INPUT_OPERATION(name, type, operation, parameters, arguments) \
  static type name parameters { \
    uint32_t expected = 0u; \
    if (!h2_jieli_atomic_cas_u32(&operation, &expected, 1u)) \
      return H2_PAL_ERR_BUSY; \
    type result = name##_impl arguments; \
    h2_jieli_atomic_store_u32(&operation, 0u); \
    return result; \
  }

/* SDK EMI posts its semaphore before calling this callback. Flush itself
 * returns zero even after a native timeout, so completion is independently
 * required before changing RS, reusing storage, or consuming the handle. */
static void display_emi_send_complete(void *device) {
  (void)device;
  h2_jieli_atomic_store_u32(&display_state.dma_done, 1u);
}

static void delay_ms(uint32_t ms) { os_time_dly(ms / 10u + (ms % 10u != 0u)); }

static int display_flush(h2_display_state_t *state) {
  if (!state->pending) return H2_DISPLAY_OK;
  uint32_t started = timer_get_ms();
  if (dev_ioctl(state->device, IOCTL_EMI_WRITE_NON_BLOCK_FLUSH, 0) != 0)
    return H2_DISPLAY_ERR_IO;
  while (!h2_jieli_atomic_load_u32(&state->dma_done)) {
    if ((uint32_t)(timer_get_ms() - started) >= 2000u)
      return H2_DISPLAY_ERR_IO;
    os_time_dly(1u);
  }
  state->pending = 0;
  return H2_DISPLAY_OK;
}

static int display_write(h2_display_state_t *state, void *bytes, uint32_t size) {
  if (display_flush(state) != H2_DISPLAY_OK) return H2_DISPLAY_ERR_IO;
  h2_jieli_atomic_store_u32(&state->dma_done, 0u);
  state->pending = 1;
  int count = dev_write(state->device, bytes, size);
  /* Pinned EMI rejects alignment before starting DMA; positive unexpected
   * progress still owns storage until its completion callback. */
  if (count <= 0) state->pending = 0;
  return count == (int)size ? H2_DISPLAY_OK : H2_DISPLAY_ERR_IO;
}

static int lcd_write(uint8_t value) {
  return display_write(&display_state, &value, 1u) == H2_DISPLAY_OK ? 0 : -1;
}

static int lcd_command(uint8_t command) {
  if (display_flush(&display_state) != H2_DISPLAY_OK) return -1;
  gpio_direction_output(H2_LCD_RS_PIN, 0);
  return lcd_write(command);
}

static int lcd_data(uint8_t value) {
  if (display_flush(&display_state) != H2_DISPLAY_OK) return -1;
  gpio_direction_output(H2_LCD_RS_PIN, 1);
  return lcd_write(value);
}

static int run_init_sequence(
    const h2_panel_init_t *sequence, size_t count) {
  for (size_t i = 0u; i < count; ++i) {
    if (sequence[i].command == UINT8_C(0x45)) {
      delay_ms(sequence[i].count);
      continue;
    }
    if (lcd_command(sequence[i].command) != 0) return -1;
    for (uint8_t j = 0u; j < sequence[i].count; ++j) {
      if (lcd_data(sequence[i].data[j]) != 0) return -1;
    }
  }
  return 0;
}

static int init_ili9481(void) {
  static const h2_panel_init_t sequence[] = {
      {0x01, 0, {0}}, {0x45, 200, {0}}, {0x13, 1, {0x00}},
      {0x35, 1, {0x00}}, {0x44, 2, {0x01, 0x50}},
      {0xc5, 1, {0x04}}, {0xc5, 1, {0x07}}, {0xe4, 1, {0xa0}},
      {0xd0, 3, {0x05, 0x40, 0x08}}, {0xd1, 3, {0x00, 0x00, 0x10}},
      {0xd2, 2, {0x01, 0x00}}, {0xc0, 5, {0x00, 0x3b, 0x00, 0x02, 0x11}},
      {0xc8, 12, {0x00, 0x26, 0x21, 0x00, 0x00, 0x1f,
                  0x65, 0x23, 0x77, 0x00, 0x0f, 0x00}},
      {0x3a, 1, {0x55}}, {0x36, 1, {0x2c}}, {0x11, 0, {0}},
      {0x45, 200, {0}}, {0x29, 0, {0}},
  };
  return run_init_sequence(sequence, sizeof(sequence) / sizeof(sequence[0]));
}

static int init_ili9488(void) {
  static const h2_panel_init_t sequence[] = {
      {0x21, 1, {0x00}}, {0x13, 1, {0x00}}, {0x35, 1, {0x00}},
      {0xb1, 2, {0x90, 0x11}}, {0x36, 1, {0xb8}}, {0xc1, 1, {0x41}},
      {0xf7, 4, {0xa9, 0x51, 0x2c, 0x82}}, {0xc0, 2, {0x0f, 0x0f}},
      {0xc2, 1, {0x22}}, {0xc5, 3, {0x00, 0x53, 0x80}},
      {0xb4, 1, {0x02}}, {0xb7, 1, {0xc6}}, {0xb6, 2, {0x02, 0x42}},
      {0xbe, 2, {0x00, 0x04}}, {0xe9, 1, {0x00}},
      {0x3a, 1, {0x55}}, {0x11, 0, {0}}, {0x45, 200, {0}},
      {0x29, 0, {0}},
  };
  return run_init_sequence(sequence, sizeof(sequence) / sizeof(sequence[0]));
}

static int display_open_impl(void *user) {
  h2_display_state_t *state = user;
  if (state->open) return H2_DISPLAY_OK;
  if (state->device != NULL) return H2_DISPLAY_ERR_INVALID_STATE;
  gpio_set_direction(H2_LCD_CHECK_D6_PIN, 1);
  gpio_set_direction(H2_LCD_CHECK_D7_PIN, 1);
  int use_ili9488 = gpio_read(H2_LCD_CHECK_D6_PIN) &&
                    gpio_read(H2_LCD_CHECK_D7_PIN);
  state->device = dev_open("emi", NULL);
  if (state->device == NULL) return H2_DISPLAY_ERR_UNAVAILABLE;
  /* JieLi's DevKit LCD driver waits for EMI completion through the driver's
   * send semaphore.  Without this mode a synchronous dev_write can remain in
   * the EMI busy wait once DAC interrupts are active, which freezes A/V on
   * the second frame. */
  if (dev_ioctl(
          state->device, EMI_SET_ISR_CB, (uintptr_t)display_emi_send_complete) != 0) {
    dev_close(state->device);
    state->device = NULL;
    return H2_DISPLAY_ERR_IO;
  }
  if (dev_ioctl(state->device, EMI_USE_SEND_SEM, 1) != 0) {
    dev_close(state->device);
    state->device = NULL;
    return H2_DISPLAY_ERR_IO;
  }
  if (dev_ioctl(state->device, IOCTL_EMI_WRITE_NON_BLOCK, 1) != 0) {
    dev_close(state->device);
    state->device = NULL;
    return H2_DISPLAY_ERR_IO;
  }
  gpio_direction_output(H2_LCD_BACKLIGHT_PIN, 1);
  gpio_direction_output(H2_LCD_RS_PIN, 1);
  gpio_direction_output(H2_LCD_RESET_PIN, 1);
  delay_ms(60u);
  gpio_direction_output(H2_LCD_RESET_PIN, 0);
  delay_ms(10u);
  gpio_direction_output(H2_LCD_RESET_PIN, 1);
  delay_ms(100u);
  if ((use_ili9488 ? init_ili9488() : init_ili9481()) != 0) {
    if (display_flush(state) != H2_DISPLAY_OK) return H2_DISPLAY_ERR_IO;
    dev_close(state->device);
    state->device = NULL;
    return H2_DISPLAY_ERR_IO;
  }
  state->open = 1;
  gpio_direction_output(H2_LCD_BACKLIGHT_PIN, 0);
  return H2_DISPLAY_OK;
}

static int display_get_info_impl(void *user, h2_display_info_t *info) {
  if (info == NULL) return H2_DISPLAY_ERR_INVALID_ARG;
  h2_display_state_t *state = user;
  if (!state->open) return H2_DISPLAY_ERR_INVALID_STATE;
  *info = (h2_display_info_t){
      .width = H2_LCD_WIDTH,
      .height = H2_LCD_HEIGHT,
      .native_format = H2_DISPLAY_PIXEL_RGB565,
  };
  return H2_DISPLAY_OK;
}

static int set_window(const h2_display_rect_t *rect) {
  uint16_t x0 = (uint16_t)rect->x;
  uint16_t y0 = (uint16_t)rect->y;
  uint16_t x1 = (uint16_t)(rect->x + rect->width - 1);
  uint16_t y1 = (uint16_t)(rect->y + rect->height - 1);
  if (lcd_command(0x2a) != 0 || lcd_data((uint8_t)(x0 >> 8u)) != 0 ||
      lcd_data((uint8_t)x0) != 0 || lcd_data((uint8_t)(x1 >> 8u)) != 0 ||
      lcd_data((uint8_t)x1) != 0 || lcd_command(0x2b) != 0 ||
      lcd_data((uint8_t)(y0 >> 8u)) != 0 || lcd_data((uint8_t)y0) != 0 ||
      lcd_data((uint8_t)(y1 >> 8u)) != 0 || lcd_data((uint8_t)y1) != 0) {
    return -1;
  }
  return lcd_command(0x2c);
}

static int display_draw_bitmap_impl(
    void *user, const h2_display_rect_t *rect, const void *pixels,
    size_t stride_bytes, h2_display_pixel_format_t format) {
  h2_display_state_t *state = user;
  if (!state->open) return H2_DISPLAY_ERR_INVALID_STATE;
  if (rect == NULL || pixels == NULL || format != H2_DISPLAY_PIXEL_RGB565 ||
      rect->x < 0 || rect->y < 0 || rect->width <= 0 || rect->height <= 0 ||
      rect->x >= H2_LCD_WIDTH || rect->y >= H2_LCD_HEIGHT ||
      rect->width > H2_LCD_WIDTH - rect->x ||
      rect->height > H2_LCD_HEIGHT - rect->y ||
      stride_bytes < (size_t)rect->width * 2u) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  /* Official JieLi LCD fills wait for the preceding non-blocking packet at
   * the beginning of the next fill, then submit the entire new image once. */
  if (display_flush(state) != H2_DISPLAY_OK) {
    return H2_DISPLAY_ERR_IO;
  }
  if (set_window(rect) != 0) return H2_DISPLAY_ERR_IO;
  gpio_direction_output(H2_LCD_RS_PIN, 1);
  const uint8_t *row = pixels;
  uint8_t *destination = state->frame;
  for (int y = 0; y < rect->height; ++y) {
    const uint16_t *source = (const uint16_t *)row;
    for (int x = 0; x < rect->width; ++x) {
      uint16_t pixel = source[x];
      *destination++ = (uint8_t)(pixel >> 8u);
      *destination++ = (uint8_t)pixel;
    }
    row += stride_bytes;
  }
  size_t bytes = (size_t)rect->width * (size_t)rect->height * 2u;
  return display_write(state, state->frame, (uint32_t)bytes);
}

static int display_present_impl(void *user) {
  return ((h2_display_state_t *)user)->open ? H2_DISPLAY_OK
                                            : H2_DISPLAY_ERR_INVALID_STATE;
}

static int display_set_brightness_impl(void *user, uint32_t percent) {
  if (!((h2_display_state_t *)user)->open || percent > 100u) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  gpio_direction_output(H2_LCD_BACKLIGHT_PIN, percent == 0u ? 1 : 0);
  return H2_DISPLAY_OK;
}

static int display_close_impl(void *user) {
  h2_display_state_t *state = user;
  if (state->device == NULL) return H2_DISPLAY_OK;
  if (display_flush(state) != H2_DISPLAY_OK) return H2_DISPLAY_ERR_IO;
  gpio_direction_output(H2_LCD_BACKLIGHT_PIN, 1);
  int result = dev_close(state->device);
  /* dev_close consumes the SDK reference before returning driver errors. */
  state->device = NULL;
  state->open = 0;
  return result == 0 ? H2_DISPLAY_OK : H2_DISPLAY_ERR_IO;
}

INPUT_OPERATION(display_open, int, display_operation,
    (void *user), (user))
INPUT_OPERATION(display_get_info, int, display_operation,
    (void *user, h2_display_info_t *info), (user, info))
INPUT_OPERATION(display_draw_bitmap, int, display_operation,
    (void *user, const h2_display_rect_t *rect, const void *pixels, size_t stride_bytes, h2_display_pixel_format_t format), (user, rect, pixels, stride_bytes, format))
INPUT_OPERATION(display_present, int, display_operation,
    (void *user), (user))
INPUT_OPERATION(display_set_brightness, int, display_operation,
    (void *user, uint32_t percent), (user, percent))
INPUT_OPERATION(display_close, int, display_operation,
    (void *user), (user))

const h2_pal_display_api_t *h2_jieli_ac791n_devkit_display_api(void) {
  static const h2_pal_display_vtable_t vtable = {
      .open = display_open,
      .get_info = display_get_info,
      .draw_bitmap = display_draw_bitmap,
      .present = display_present,
      .set_brightness_percent = display_set_brightness,
      .close = display_close,
  };
  static const h2_pal_display_api_t api = {
      .user = &display_state, .vtable = &vtable};
  return &api;
}

static int touch_write_register(uint8_t reg, uint8_t value) {
  int result = dev_ioctl(touch_state.iic, IIC_IOCTL_START, 0) == 0 ? 0 : -1;
  if (result == 0 && (dev_ioctl(touch_state.iic, IIC_IOCTL_TX_WITH_START_BIT, H2_FT6236_WRITE) ||
      dev_ioctl(touch_state.iic, IIC_IOCTL_TX, reg) ||
      dev_ioctl(touch_state.iic, IIC_IOCTL_TX_WITH_STOP_BIT, value))) {
    result = -1;
  }
  /* START owns the native transaction mutex even if the driver reports an
   * error; STOP must always run to release it, and its error is observable. */
  if (dev_ioctl(touch_state.iic, IIC_IOCTL_STOP, 0) != 0) result = -1;
  return result;
}

static int touch_read_register(uint8_t reg, uint8_t *value) {
  int result = dev_ioctl(touch_state.iic, IIC_IOCTL_START, 0) == 0 ? 0 : -1;
  if (result == 0 && (dev_ioctl(touch_state.iic, IIC_IOCTL_TX_WITH_START_BIT, H2_FT6236_WRITE) ||
      dev_ioctl(touch_state.iic, IIC_IOCTL_TX, reg) ||
      dev_ioctl(touch_state.iic, IIC_IOCTL_TX_WITH_START_BIT, H2_FT6236_READ) ||
      dev_ioctl(touch_state.iic, IIC_IOCTL_RX_WITH_STOP_BIT, (uintptr_t)value))) {
    result = -1;
  }
  /* START owns the native transaction mutex even if the driver reports an
   * error; STOP must always run to release it, and its error is observable. */
  if (dev_ioctl(touch_state.iic, IIC_IOCTL_STOP, 0) != 0) result = -1;
  return result;
}

static h2_pal_result_t touch_open_impl(void *user) {
  h2_touch_state_t *state = user;
  uint8_t chip_id = 0u;
  if (state->open) return H2_PAL_OK;
  gpio_set_direction(H2_TOUCH_INT_PIN, 1);
  gpio_set_pull_up(H2_TOUCH_INT_PIN, 1);
  gpio_set_pull_down(H2_TOUCH_INT_PIN, 0);
  state->iic = dev_open("iic0", NULL);
  if (state->iic == NULL) return H2_PAL_ERR_UNAVAILABLE;
  delay_ms(20u);
  if (touch_read_register(H2_FT6236_CHIP_ID, &chip_id) != 0 ||
      chip_id != UINT8_C(0x64)) {
    dev_close(state->iic);
    state->iic = NULL;
    return H2_PAL_ERR_UNAVAILABLE;
  }
  if (touch_write_register(0x00, 0x00) != 0 ||
      touch_write_register(0x80, 22) != 0 ||
      touch_write_register(0x88, 13) != 0) {
    dev_close(state->iic);
    state->iic = NULL;
    return H2_PAL_ERR_IO;
  }
  state->open = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t touch_get_info_impl(
    void *user, h2_pal_touch_info_t *out_info) {
  if (out_info == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!((h2_touch_state_t *)user)->open) return H2_PAL_ERR_INVALID_STATE;
  *out_info = (h2_pal_touch_info_t){
      .width = H2_LCD_WIDTH, .height = H2_LCD_HEIGHT};
  return H2_PAL_OK;
}

static h2_pal_result_t touch_poll_event_impl(
    void *user, h2_pal_touch_event_t *out_event) {
  if (out_event == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_touch_state_t *state = user;
  uint8_t fingers = 0u;
  uint8_t point[4];
  if (!state->open) return H2_PAL_ERR_INVALID_STATE;
  if (touch_read_register(H2_FT6236_FINGERS, &fingers) != 0) {
    return H2_PAL_ERR_IO;
  }
  if ((fingers & 0x0fu) == 0u) {
    if (!state->down) return H2_PAL_ERR_WOULD_BLOCK;
    state->down = 0;
    *out_event = (h2_pal_touch_event_t){
        .kind = H2_PAL_TOUCH_EVENT_UP, .x = state->x, .y = state->y};
    return H2_PAL_OK;
  }
  for (size_t i = 0u; i < sizeof(point); ++i) {
    if (touch_read_register((uint8_t)(H2_FT6236_POINT1 + i), &point[i]) != 0) {
      return H2_PAL_ERR_IO;
    }
  }
  uint16_t raw_x =
      (uint16_t)(((uint16_t)(point[0] & 0x0fu) << 8u) | point[1]);
  uint16_t raw_y =
      (uint16_t)(((uint16_t)(point[2] & 0x0fu) << 8u) | point[3]);
  /* Match the DevKit's official FT6236 landscape transform. Keep the PAL
   * result strictly inside its zero-based 480x320 coordinate space. */
  uint16_t x = raw_y < H2_LCD_WIDTH ? raw_y : H2_LCD_WIDTH - 1u;
  uint16_t y = raw_x < H2_LCD_HEIGHT
                   ? (uint16_t)(H2_LCD_HEIGHT - 1u - raw_x)
                   : 0u;
  if (state->down && x == state->x && y == state->y) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  h2_pal_touch_event_kind_t kind =
      state->down ? H2_PAL_TOUCH_EVENT_MOVE : H2_PAL_TOUCH_EVENT_DOWN;
  state->down = 1;
  state->x = x;
  state->y = y;
  *out_event = (h2_pal_touch_event_t){.kind = kind, .x = x, .y = y};
  return H2_PAL_OK;
}

static h2_pal_result_t touch_close_impl(void *user) {
  h2_touch_state_t *state = user;
  int result = state->iic == NULL ? 0 : dev_close(state->iic);
  *state = (h2_touch_state_t){0};
  return result == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

INPUT_OPERATION(touch_open, h2_pal_result_t, touch_operation,
    (void *user), (user))
INPUT_OPERATION(touch_get_info, h2_pal_result_t, touch_operation,
    (void *user, h2_pal_touch_info_t *info), (user, info))
INPUT_OPERATION(touch_poll_event, h2_pal_result_t, touch_operation,
    (void *user, h2_pal_touch_event_t *event), (user, event))
INPUT_OPERATION(touch_close, h2_pal_result_t, touch_operation,
    (void *user), (user))

const h2_pal_touch_api_t *h2_jieli_ac791n_devkit_touch_api(void) {
  static const h2_pal_touch_vtable_t vtable = {
      .open = touch_open,
      .get_info = touch_get_info,
      .poll_event = touch_poll_event,
      .close = touch_close,
  };
  static const h2_pal_touch_api_t api = {
      .user = &touch_state, .vtable = &vtable};
  return &api;
}

static int decode_adkey(uint16_t value) {
  enum { UPLOAD_R = 22, ADC_FULL = 0x3ff };
  static const uint16_t thresholds[] = {
      ((0) + (ADC_FULL * 3 / (3 + UPLOAD_R))) / 2,
      ((ADC_FULL * 3 / (3 + UPLOAD_R)) +
       (ADC_FULL * 75 / (75 + UPLOAD_R * 10))) / 2,
      ((ADC_FULL * 75 / (75 + UPLOAD_R * 10)) +
       (ADC_FULL * 13 / (13 + UPLOAD_R))) / 2,
      ((ADC_FULL * 13 / (13 + UPLOAD_R)) +
       (ADC_FULL * 22 / (22 + UPLOAD_R))) / 2,
      ((ADC_FULL * 22 / (22 + UPLOAD_R)) +
       (ADC_FULL * 36 / (36 + UPLOAD_R))) / 2,
      ((ADC_FULL * 36 / (36 + UPLOAD_R)) +
       (ADC_FULL * 62 / (62 + UPLOAD_R))) / 2,
      ((ADC_FULL * 62 / (62 + UPLOAD_R)) +
       (ADC_FULL * 150 / (150 + UPLOAD_R))) / 2,
      ((ADC_FULL * 150 / (150 + UPLOAD_R)) + ADC_FULL) / 2 + 50,
  };
  for (size_t i = 0u; i < sizeof(thresholds) / sizeof(thresholds[0]); ++i) {
    if (value <= thresholds[i]) return (int)i;
  }
  return -1;
}

static h2_pal_result_t init_adkeys(void) {
  uint32_t phase = h2_jieli_atomic_load_u32(&adkey_initialized);
  if (phase == 2u) return H2_PAL_OK;
  uint32_t expected = 0u;
  if (!h2_jieli_atomic_cas_u32(&adkey_initialized, &expected, 1u))
    return expected == 2u ? H2_PAL_OK : H2_PAL_ERR_BUSY;
  int result = H2_PAL_ERR_IO;
  if (gpio_set_die(H2_ADKEY_PIN, 0) == 0 &&
      gpio_set_direction(H2_ADKEY_PIN, 1) == 0 &&
      gpio_set_pull_up(H2_ADKEY_PIN, 0) == 0 &&
      gpio_set_pull_down(H2_ADKEY_PIN, 0) == 0 &&
      adc_add_sample_ch(AD_CH_PB01) < ADC_MAX_CH) {
    result = H2_PAL_OK;
  }
  h2_jieli_atomic_store_u32(&adkey_initialized, result == H2_PAL_OK ? 2u : 0u);
  return result;
}

static h2_pal_result_t read_single_button(
    void *user, h2_pal_periph_id_t id,
    h2_pal_single_button_reading_t *out_reading) {
  (void)user;
  if (out_reading == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (id < H2_JIELI_AC791N_ADKEY_POWER_ID ||
      id > H2_JIELI_AC791N_ADKEY_CANCEL_ID) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  int result = init_adkeys();
  if (result != H2_PAL_OK) return result;
  int active = decode_adkey((uint16_t)adc_get_value(AD_CH_PB01));
  *out_reading = (h2_pal_single_button_reading_t){
      .id = id,
      .state = active == (int)(id - H2_JIELI_AC791N_ADKEY_POWER_ID)
                   ? H2_PAL_BUTTON_STATE_PRESSED
                   : H2_PAL_BUTTON_STATE_RELEASED,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t read_radio_button_group(
    void *user, h2_pal_periph_id_t id,
    h2_pal_radio_button_group_reading_t *out_reading) {
  (void)user;
  if (out_reading == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (id != H2_JIELI_AC791N_ADKEY_GROUP_ID) return H2_PAL_ERR_NOT_FOUND;
  int result = init_adkeys();
  if (result != H2_PAL_OK) return result;
  int active = decode_adkey((uint16_t)adc_get_value(AD_CH_PB01));
  *out_reading = (h2_pal_radio_button_group_reading_t){
      .id = id,
      .pressed_button_id =
          active < 0 ? 0u
                     : (h2_pal_periph_id_t)(
                           H2_JIELI_AC791N_ADKEY_POWER_ID + active),
  };
  return H2_PAL_OK;
}

const h2_pal_button_api_t *h2_jieli_ac791n_devkit_button_api(void) {
  static const h2_pal_button_vtable_t vtable = {
      .read_single_button = read_single_button,
      .read_radio_button_group = read_radio_button_group,
  };
  static const h2_pal_button_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}
