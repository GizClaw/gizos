#include "asm/includes.h"
#include "asm/adc_api.h"
#include "device/device.h"
#include "device/iic.h"
#include "device/ioctl_cmds.h"
#include "gpio.h"
#include "os/os_api.h"

#include "h2_jieli_ac791n_devkit.h"

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
static int adkey_initialized;

/* JieLi's LCD setup registers an EMI completion callback before enabling
 * non-blocking writes. The PAL waits through the driver's send semaphore, so
 * this callback does not need a second higher-level notification. */
static void display_emi_send_complete(void) {}

static void delay_ms(uint32_t ms) { os_time_dly((ms + 9u) / 10u); }

static int lcd_write(uint8_t value) {
  return dev_write(display_state.device, &value, 1u) == 1 ? 0 : -1;
}

static int lcd_command(uint8_t command) {
  gpio_direction_output(H2_LCD_RS_PIN, 0);
  return lcd_write(command);
}

static int lcd_data(uint8_t value) {
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

static int display_open(void *user) {
  h2_display_state_t *state = user;
  if (state->open) return H2_DISPLAY_OK;
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
          state->device, EMI_SET_ISR_CB, display_emi_send_complete) != 0) {
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
    dev_close(state->device);
    state->device = NULL;
    return H2_DISPLAY_ERR_IO;
  }
  state->open = 1;
  gpio_direction_output(H2_LCD_BACKLIGHT_PIN, 0);
  return H2_DISPLAY_OK;
}

static int display_get_info(void *user, h2_display_info_t *info) {
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

static int display_draw_bitmap(
    void *user, const h2_display_rect_t *rect, const void *pixels,
    size_t stride_bytes, h2_display_pixel_format_t format) {
  h2_display_state_t *state = user;
  if (!state->open) return H2_DISPLAY_ERR_INVALID_STATE;
  if (rect == NULL || pixels == NULL || format != H2_DISPLAY_PIXEL_RGB565 ||
      rect->x < 0 || rect->y < 0 || rect->width <= 0 || rect->height <= 0 ||
      rect->x + rect->width > H2_LCD_WIDTH ||
      rect->y + rect->height > H2_LCD_HEIGHT ||
      rect->width > H2_LCD_WIDTH || stride_bytes < (size_t)rect->width * 2u) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  /* Official JieLi LCD fills wait for the preceding non-blocking packet at
   * the beginning of the next fill, then submit the entire new image once. */
  if (dev_ioctl(
          state->device, IOCTL_EMI_WRITE_NON_BLOCK_FLUSH, 0) != 0) {
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
  return dev_write(state->device, state->frame, (uint32_t)bytes) == (int)bytes
             ? H2_DISPLAY_OK
             : H2_DISPLAY_ERR_IO;
}

static int display_present(void *user) {
  return ((h2_display_state_t *)user)->open ? H2_DISPLAY_OK
                                            : H2_DISPLAY_ERR_INVALID_STATE;
}

static int display_set_brightness(void *user, uint32_t percent) {
  if (!((h2_display_state_t *)user)->open || percent > 100u) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  gpio_direction_output(H2_LCD_BACKLIGHT_PIN, percent == 0u ? 1 : 0);
  return H2_DISPLAY_OK;
}

static int display_close(void *user) {
  h2_display_state_t *state = user;
  if (!state->open) return H2_DISPLAY_OK;
  gpio_direction_output(H2_LCD_BACKLIGHT_PIN, 1);
  (void)dev_ioctl(state->device, IOCTL_EMI_WRITE_NON_BLOCK_FLUSH, 0);
  dev_close(state->device);
  state->device = NULL;
  state->open = 0;
  return H2_DISPLAY_OK;
}

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
  int result = 0;
  dev_ioctl(touch_state.iic, IIC_IOCTL_START, 0);
  if (dev_ioctl(touch_state.iic, IIC_IOCTL_TX_WITH_START_BIT, H2_FT6236_WRITE) ||
      dev_ioctl(touch_state.iic, IIC_IOCTL_TX, reg) ||
      dev_ioctl(touch_state.iic, IIC_IOCTL_TX_WITH_STOP_BIT, value)) {
    result = -1;
  }
  dev_ioctl(touch_state.iic, IIC_IOCTL_STOP, 0);
  return result;
}

static int touch_read_register(uint8_t reg, uint8_t *value) {
  int result = 0;
  dev_ioctl(touch_state.iic, IIC_IOCTL_START, 0);
  if (dev_ioctl(touch_state.iic, IIC_IOCTL_TX_WITH_START_BIT, H2_FT6236_WRITE) ||
      dev_ioctl(touch_state.iic, IIC_IOCTL_TX, reg) ||
      dev_ioctl(touch_state.iic, IIC_IOCTL_TX_WITH_START_BIT, H2_FT6236_READ) ||
      dev_ioctl(touch_state.iic, IIC_IOCTL_RX_WITH_STOP_BIT, (uint32_t)value)) {
    result = -1;
  }
  dev_ioctl(touch_state.iic, IIC_IOCTL_STOP, 0);
  return result;
}

static h2_pal_result_t touch_open(void *user) {
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
  (void)touch_write_register(0x00, 0x00);
  (void)touch_write_register(0x80, 22);
  (void)touch_write_register(0x88, 13);
  state->open = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t touch_get_info(
    void *user, h2_pal_touch_info_t *out_info) {
  if (!((h2_touch_state_t *)user)->open) return H2_PAL_ERR_INVALID_STATE;
  *out_info = (h2_pal_touch_info_t){
      .width = H2_LCD_WIDTH, .height = H2_LCD_HEIGHT};
  return H2_PAL_OK;
}

static h2_pal_result_t touch_poll_event(
    void *user, h2_pal_touch_event_t *out_event) {
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

static h2_pal_result_t touch_close(void *user) {
  h2_touch_state_t *state = user;
  if (state->iic != NULL) dev_close(state->iic);
  *state = (h2_touch_state_t){0};
  return H2_PAL_OK;
}

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

static void init_adkeys(void) {
  if (adkey_initialized) return;
  gpio_set_die(H2_ADKEY_PIN, 0);
  gpio_set_direction(H2_ADKEY_PIN, 1);
  gpio_set_pull_up(H2_ADKEY_PIN, 0);
  gpio_set_pull_down(H2_ADKEY_PIN, 0);
  (void)adc_add_sample_ch(AD_CH_PB01);
  adkey_initialized = 1;
}

static h2_pal_result_t read_single_button(
    void *user, h2_pal_periph_id_t id,
    h2_pal_single_button_reading_t *out_reading) {
  (void)user;
  if (id < H2_JIELI_AC791N_ADKEY_POWER_ID ||
      id > H2_JIELI_AC791N_ADKEY_CANCEL_ID) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  init_adkeys();
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
  if (id != H2_JIELI_AC791N_ADKEY_GROUP_ID) return H2_PAL_ERR_NOT_FOUND;
  init_adkeys();
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
