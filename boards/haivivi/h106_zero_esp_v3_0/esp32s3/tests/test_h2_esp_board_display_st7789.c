/* ST7789 provider validation and RGB565 wire-format coverage.
 * The production provider links against ESP shadow headers and SDK stubs;
 * transfer captures verify pixels without driving a physical panel. */

#include "h2_esp_board.h"
#include "h2_esp_board_private.h"
#include "h2_esp_zero_esp_board_internal.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/spi_struct.h"
#include "freertos/task.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- ESP SDK stubs ----------------------------------------------------- */

uint32_t esp_log_timestamp(void) { return 0u; }
static int64_t s_now_us;
int64_t esp_timer_get_time(void) { return s_now_us; }
static esp_lcd_panel_io_color_trans_done_cb_t s_color_done_callback;
static esp_err_t s_color_result = ESP_OK;
spi_dev_t GPSPI2;
const char* esp_err_to_name(esp_err_t code) {
  (void)code;
  return "stub";
}
esp_err_t gpio_config(const gpio_config_t* cfg) {
  (void)cfg;
  return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level) {
  (void)gpio;
  (void)level;
  return ESP_OK;
}
int gpio_get_level(gpio_num_t gpio) {
  (void)gpio;
  return 0;
}
esp_err_t ledc_timer_config(const ledc_timer_config_t* cfg) {
  (void)cfg;
  return ESP_OK;
}
esp_err_t ledc_channel_config(const ledc_channel_config_t* cfg) {
  (void)cfg;
  return ESP_OK;
}
static size_t s_color_tx_count;
static unsigned s_sleep_in_count, s_sleep_out_count;
static unsigned s_display_on_count, s_display_off_count;
static size_t s_color_tx_count_at_display_on;
static bool s_display_on_before_clear;
static bool s_backlight_enabled_before_display_on;
static bool s_sleep_in_before_display_off;
static bool s_check_sleep_before_backlight;
static bool s_backlight_enabled_before_color;
static uint32_t s_backlight_duty;
static unsigned s_backlight_call_count;
esp_err_t ledc_set_duty(ledc_mode_t m, ledc_channel_t c, uint32_t d) {
  (void)m;
  (void)c;
  s_backlight_duty = d;
  ++s_backlight_call_count;
  if (s_check_sleep_before_backlight && d == 0u) assert(s_sleep_in_count == 1u);
  if (d > 0u && s_color_tx_count == 0u) {
    s_backlight_enabled_before_color = true;
  }
  if (d > 0u && s_display_on_count == 0u) {
    s_backlight_enabled_before_display_on = true;
  }
  return ESP_OK;
}
esp_err_t ledc_update_duty(ledc_mode_t m, ledc_channel_t c) {
  (void)m;
  (void)c;
  ++s_backlight_call_count;
  return ESP_OK;
}
esp_err_t spi_bus_initialize(spi_host_device_t h, const spi_bus_config_t* c,
                             spi_common_dma_t d) {
  (void)h;
  (void)c;
  (void)d;
  return ESP_OK;
}
static int s_fake_panel_io;
static uint8_t s_colmod;
static uint8_t s_pixels[7680];
static size_t s_pixel_bytes;

esp_err_t esp_lcd_new_panel_io_spi(
    esp_lcd_spi_bus_handle_t bus,
    const esp_lcd_panel_io_spi_config_t* io_config,
    esp_lcd_panel_io_handle_t* ret_io) {
  (void)bus;
  s_color_done_callback = io_config->on_color_trans_done;
  assert(s_color_done_callback != NULL);
  *ret_io = (esp_lcd_panel_io_handle_t)(void*)&s_fake_panel_io;
  return ESP_OK;
}
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int cmd,
                                    const void* p, size_t n) {
  (void)io;
  if (cmd == 0x10) {
    ++s_sleep_in_count;
    if (s_display_off_count == 0u) s_sleep_in_before_display_off = true;
  }
  if (cmd == 0x28) ++s_display_off_count;
  if (cmd == 0x29) {
    ++s_display_on_count;
    s_color_tx_count_at_display_on = s_color_tx_count;
    if (s_color_tx_count < 15u) s_display_on_before_clear = true;
  }
  if (cmd == 0x11) ++s_sleep_out_count;
  if (cmd == 0x3a && n == 1u) s_colmod = *(const uint8_t*)p;
  return ESP_OK;
}
esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int cmd,
                                    const void* c, size_t n) {
  (void)io;
  (void)cmd;
  if (s_color_result != ESP_OK) return s_color_result;
  assert(n <= sizeof(s_pixels));
  memcpy(s_pixels, c, n);
  s_pixel_bytes = n;
  ++s_color_tx_count;
  return ESP_OK;
}
size_t heap_caps_get_free_size(uint32_t caps) {
  (void)caps;
  return 0u;
}
size_t heap_caps_get_largest_free_block(uint32_t caps) {
  (void)caps;
  return 0u;
}
bool esp_ptr_internal(const void* p) {
  (void)p;
  return true;
}
bool esp_ptr_dma_capable(const void* p) {
  (void)p;
  return true;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; }

/* ---- board stubs the provider links against ---------------------------- */

i2c_master_bus_handle_t h2_esp_zero_esp_board_i2c_bus(void) { return NULL; }
int h2_esp_zero_esp_board_enable_peripheral_power(void) { return 0; }

/* ---- tests ------------------------------------------------------------- */

static const h2_pal_display_vtable_t* display_vtable(
    h2_pal_display_t* display) {
  assert(display != NULL);
  assert(display->vtable != NULL);
  return display->vtable;
}

static void test_draw_bitmap_rejects_a_null_rect_before_opening(void) {
  h2_pal_display_t* display = h2_esp_board_display();
  const h2_pal_display_vtable_t* vtable = display_vtable(display);
  assert(vtable->draw_bitmap != NULL);

  uint8_t pixels[16] = {0};
  /* Before opening, the state check is the one that fires. */
  assert(vtable->draw_bitmap(display->user, NULL, pixels, 4u,
                             H2_DISPLAY_PIXEL_RGB565) ==
         H2_DISPLAY_ERR_INVALID_STATE);
}

static void test_open_clears_panel_before_enabling_backlight(void) {
  h2_pal_display_t* display = h2_esp_board_display();
  const h2_pal_display_vtable_t* vtable = display_vtable(display);
  s_color_tx_count = 0u;
  s_backlight_enabled_before_color = false;

  assert(vtable->open(display->user) == H2_DISPLAY_OK);
  assert(s_color_tx_count == 15u);
  assert(s_colmod == 0x05u);
  assert(s_pixel_bytes == 7680u);
  assert(!s_backlight_enabled_before_color);
  assert(s_display_on_count == 1u);
  assert(s_color_tx_count_at_display_on == 15u);
  assert(!s_display_on_before_clear);
  assert(!s_backlight_enabled_before_display_on);
}

static void test_draw_bitmap_rejects_a_null_rect_when_open(void) {
  h2_pal_display_t* display = h2_esp_board_display();
  const h2_pal_display_vtable_t* vtable = display_vtable(display);
  assert(vtable->open(display->user) == H2_DISPLAY_OK);

  uint8_t pixels[16] = {0};
  /* Without the guard this dereferences NULL inside clip_rect(). */
  assert(vtable->draw_bitmap(display->user, NULL, pixels, 4u,
                             H2_DISPLAY_PIXEL_RGB565) ==
         H2_DISPLAY_ERR_INVALID_ARG);
}

static void test_draw_bitmap_rejects_null_pixels_when_open(void) {
  h2_pal_display_t* display = h2_esp_board_display();
  const h2_pal_display_vtable_t* vtable = display_vtable(display);
  assert(vtable->open(display->user) == H2_DISPLAY_OK);

  const h2_display_rect_t rect = {.x = 0, .y = 0, .width = 2, .height = 2};
  assert(vtable->draw_bitmap(display->user, &rect, NULL, 4u,
                             H2_DISPLAY_PIXEL_RGB565) ==
         H2_DISPLAY_ERR_INVALID_ARG);
}

static void test_source_span_overflow_and_single_row_boundary(void) {
  h2_pal_display_t* display = h2_esp_board_display();
  const h2_pal_display_vtable_t* vtable = display_vtable(display);
  h2_display_info_t info;
  assert(vtable->get_info(display->user, &info) == H2_DISPLAY_OK);
  const uint16_t pixel = 0xf800u;
  h2_display_rect_t rect = {
      .x = 0, .y = info.height - 1, .width = 1, .height = 3};
  const size_t before = s_color_tx_count;
  /* Bottom clipping cannot make the original three-row span valid. */
  assert(vtable->draw_bitmap(display->user, &rect, &pixel, SIZE_MAX,
                             H2_DISPLAY_PIXEL_RGB565) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(s_color_tx_count == before);

  /* A single row never advances by its stride, even at SIZE_MAX. */
  rect.height = 1;
  assert(vtable->draw_bitmap(display->user, &rect, &pixel, SIZE_MAX,
                             H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_OK);
  const uint8_t expected[] = {0xf8, 0x00};
  assert(s_pixel_bytes == sizeof(expected));
  assert(memcmp(s_pixels, expected, sizeof(expected)) == 0);
}

static void test_rgb565_wire_bytes_and_clipping(void) {
  h2_pal_display_t* display = h2_esp_board_display();
  const h2_pal_display_vtable_t* vtable = display_vtable(display);
  const uint16_t pixels[] = {0xffff, 0xf800, 0x07e0, 0xdead,
                             0xffff, 0x001f, 0x1234, 0xbeef};
  const h2_display_rect_t rect = {.x = -1, .y = 0, .width = 3, .height = 2};
  const uint8_t expected[] = {0xf8, 0x00, 0x07, 0xe0, 0x00, 0x1f, 0x12, 0x34};
  assert(vtable->draw_bitmap(display->user, &rect, pixels, 8u,
                             H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_OK);
  assert(s_pixel_bytes == sizeof(expected));
  assert(memcmp(s_pixels, expected, sizeof(expected)) == 0);
}

static void test_other_pal_formats_encode_rgb565(void) {
  h2_pal_display_t* display = h2_esp_board_display();
  const h2_pal_display_vtable_t* vtable = display_vtable(display);
  const h2_display_rect_t rect = {.x = 0, .y = 0, .width = 3, .height = 1};
  const uint8_t rgb888[] = {255, 0, 0, 0, 255, 0, 0, 0, 255};
  const uint16_t rgb444[] = {0x0f00, 0x00f0, 0x000f};
  const uint8_t expected[] = {0xf8, 0x00, 0x07, 0xe0, 0x00, 0x1f};
  assert(vtable->draw_bitmap(display->user, &rect, rgb888, sizeof(rgb888),
                             H2_DISPLAY_PIXEL_RGB888) == H2_DISPLAY_OK);
  assert(s_pixel_bytes == sizeof(expected));
  assert(memcmp(s_pixels, expected, sizeof(expected)) == 0);
  assert(vtable->draw_bitmap(display->user, &rect, rgb444, sizeof(rgb444),
                             H2_DISPLAY_PIXEL_RGB444) == H2_DISPLAY_OK);
  assert(s_pixel_bytes == sizeof(expected));
  assert(memcmp(s_pixels, expected, sizeof(expected)) == 0);
}

static void test_brightness_bounds_preserve_pwm_on_invalid_values(void) {
  h2_pal_display_t* display = h2_esp_board_display();
  const h2_pal_display_vtable_t* vtable = display_vtable(display);
  assert(vtable->set_brightness_percent(display->user, 50u) == H2_DISPLAY_OK);
  const uint32_t half_duty = s_backlight_duty;
  const unsigned before = s_backlight_call_count;
  assert(vtable->set_brightness_percent(display->user, 101u) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(vtable->set_brightness_percent(display->user, UINT32_MAX) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(s_backlight_call_count == before);
  assert(s_backlight_duty == half_duty);
  assert(vtable->set_brightness_percent(display->user, 0u) == H2_DISPLAY_OK);
  assert(s_backlight_duty == 0u);
  assert(vtable->set_brightness_percent(display->user, 100u) == H2_DISPLAY_OK);
  assert(s_backlight_duty > half_duty);
}

static void test_diagnostics_before_first_transfer(void) {
  char out[320];
  assert(h2_esp_board_display_diagnostics(out, sizeof(out)) == strlen(out));
  assert(
      strstr(out,
             "color_queued=0 color_done=0 queued_age_ms=-1 done_age_ms=-1") !=
      NULL);
  assert(h2_esp_board_display_diagnostics(NULL, sizeof(out)) == 0u);
  out[0] = 'x';
  assert(h2_esp_board_display_diagnostics(out, 0u) == 0u);
  assert(out[0] == 'x');
  assert(h2_esp_board_display_diagnostics(out, 1u) == 0u);
  assert(out[0] == '\0');
}

static void test_diagnostics_observe_dma_progress_and_bound_output(void) {
  char out[320];
  h2_pal_display_t* display = h2_esp_board_display();
  const h2_display_rect_t rect = {.x = 0, .y = 0, .width = 1, .height = 1};
  const uint16_t pixel = 0xffffu;
  s_now_us = 1000000;
  assert(h2_pal_display_draw_bitmap(display, &rect, &pixel, sizeof(pixel),
                                    H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_OK);
  GPSPI2.cmd.usr = 1u;
  GPSPI2.dma_int_raw.val = 0x12u;
  GPSPI2.dma_int_ena.val = 0x34u;
  GPSPI2.dma_int_st.val = 0x10u;
  s_now_us = 1250000;
  size_t length = h2_esp_board_display_diagnostics(out, sizeof(out));
  assert(length == strlen(out));
  char expected[80];
  snprintf(expected, sizeof(expected), "color_queued=%lu color_done=0 ",
           (unsigned long)s_color_tx_count);
  assert(strstr(out, expected) != NULL);
  assert(strstr(out, "queued_age_ms=250 done_age_ms=-1") != NULL);
  assert(strstr(out,
                "spi2_usr=1 spi2_dma_int_raw=0x00000012 "
                "spi2_dma_int_ena=0x00000034 spi2_dma_int_st=0x00000010") !=
         NULL);
  assert(GPSPI2.dma_int_raw.val == 0x12u);
  assert(!s_color_done_callback((esp_lcd_panel_io_handle_t)&s_fake_panel_io,
                                NULL, NULL));
  s_now_us = 1400000;
  h2_esp_board_display_diagnostics(out, sizeof(out));
  assert(strstr(out, "color_done=1 queued_age_ms=400 done_age_ms=150") != NULL);

  /* A rejected transfer must not claim another queued block. */
  s_color_result = ESP_ERR_NO_MEM;
  assert(h2_pal_display_draw_bitmap(display, &rect, &pixel, sizeof(pixel),
                                    H2_DISPLAY_PIXEL_RGB565) ==
         H2_DISPLAY_ERR_NO_MEMORY);
  s_color_result = ESP_OK;
  h2_esp_board_display_diagnostics(out, sizeof(out));
  snprintf(expected, sizeof(expected), "color_queued=%lu color_done=1 ",
           (unsigned long)s_color_tx_count);
  assert(strstr(out, expected) != NULL);

  char short_out[9];
  memset(short_out, 'x', sizeof(short_out));
  assert(h2_esp_board_display_diagnostics(short_out, 8u) == 7u);
  assert(short_out[7] == '\0' && short_out[8] == 'x');

  /* Timestamps retain precision beyond the 32-bit millisecond range. */
  s_now_us = ((int64_t)UINT32_MAX - 10) * 1000;
  assert(h2_pal_display_draw_bitmap(display, &rect, &pixel, sizeof(pixel),
                                    H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_OK);
  assert(!s_color_done_callback((esp_lcd_panel_io_handle_t)&s_fake_panel_io,
                                NULL, NULL));
  s_now_us += 25000;
  h2_esp_board_display_diagnostics(out, sizeof(out));
  assert(strstr(out, "queued_age_ms=25 done_age_ms=25") != NULL);
}

int main(void) {
  test_diagnostics_before_first_transfer();
  test_draw_bitmap_rejects_a_null_rect_before_opening();
  test_open_clears_panel_before_enabling_backlight();
  test_draw_bitmap_rejects_a_null_rect_when_open();
  test_draw_bitmap_rejects_null_pixels_when_open();
  test_source_span_overflow_and_single_row_boundary();
  test_rgb565_wire_bytes_and_clipping();
  test_other_pal_formats_encode_rgb565();
  test_brightness_bounds_preserve_pwm_on_invalid_values();
  test_diagnostics_observe_dma_progress_and_bound_output();
  h2_pal_display_t* display = h2_esp_board_display();
  s_check_sleep_before_backlight = true;
  assert(h2_pal_display_close(display) == H2_DISPLAY_OK);
  assert(s_sleep_in_count == 1u);
  assert(s_display_off_count == 1u);
  assert(!s_sleep_in_before_display_off);
  assert(h2_pal_display_close(display) == H2_DISPLAY_OK);
  assert(s_sleep_in_count == 1u);
  assert(s_display_off_count == 1u);
  assert(!s_sleep_in_before_display_off);
  s_check_sleep_before_backlight = false;
  s_color_tx_count = 0u;
  s_display_on_count = 0u;
  s_display_on_before_clear = false;
  s_backlight_enabled_before_display_on = false;
  s_backlight_enabled_before_color = false;
  unsigned before = s_sleep_out_count;
  assert(h2_pal_display_open(display) == H2_DISPLAY_OK);
  assert(s_sleep_out_count == before + 1u);
  assert(s_color_tx_count == 15u);
  assert(s_display_on_count == 1u);
  assert(s_color_tx_count_at_display_on == 15u);
  assert(!s_display_on_before_clear);
  assert(!s_backlight_enabled_before_display_on);
  assert(!s_backlight_enabled_before_color);
  return 0;
}
