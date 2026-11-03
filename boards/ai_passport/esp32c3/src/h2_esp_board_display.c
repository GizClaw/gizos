#include "h2_esp_board_private.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include <string.h>

enum { WIDTH = 240, HEIGHT = 320, ROWS = 4 };
static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t panel_io;
static uint16_t* transfer;
static int spi_ready, timer_ready, light_ready, opened;

int h2_passport_display_deinit(void) {
  esp_err_t err;
  opened = 0;
  if (panel != NULL) {
    err = esp_lcd_panel_del(panel);
    if (err != ESP_OK) return H2_PAL_ERR_IO;
    panel = NULL;
  }
  if (panel_io != NULL) {
    err = esp_lcd_panel_io_del(panel_io);
    if (err != ESP_OK) return H2_PAL_ERR_IO;
    panel_io = NULL;
  }
  if (spi_ready) {
    if (spi_bus_free(SPI2_HOST) != ESP_OK) return H2_PAL_ERR_IO;
    spi_ready = 0;
  }
  if (light_ready) {
    if (ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0) != ESP_OK)
      return H2_PAL_ERR_IO;
    light_ready = 0;
  }
  if (timer_ready) {
    if (ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0) != ESP_OK)
      return H2_PAL_ERR_IO;
    timer_ready = 0;
  }
  heap_caps_free(transfer);
  transfer = NULL;
  return 0;
}

static int screen_open(void* user) {
  (void)user;
  if (opened) return 0;
  int cleanup = h2_passport_display_deinit();
  if (cleanup != 0) return cleanup;
  const spi_bus_config_t bus = {.mosi_io_num = 9,
                                .miso_io_num = -1,
                                .sclk_io_num = 8,
                                .quadwp_io_num = -1,
                                .quadhd_io_num = -1,
                                .max_transfer_sz = WIDTH * ROWS * 2};
  esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) return H2_PAL_ERR_IO;
  spi_ready = 1;
  const esp_lcd_panel_io_spi_config_t io = {.dc_gpio_num = 20,
                                            .cs_gpio_num = 1,
                                            .pclk_hz = 40000000,
                                            .spi_mode = 0,
                                            .trans_queue_depth = 1,
                                            .lcd_cmd_bits = 8,
                                            .lcd_param_bits = 8};
  err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io,
                                 &panel_io);
  const esp_lcd_panel_dev_config_t config = {
      .reset_gpio_num = -1,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = 16,
      .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE};
  if (err == ESP_OK) err = esp_lcd_new_panel_st7789(panel_io, &config, &panel);
  if (err == ESP_OK) err = esp_lcd_panel_reset(panel);
  if (err == ESP_OK) err = esp_lcd_panel_init(panel);
  if (err == ESP_OK) err = esp_lcd_panel_invert_color(panel, true);
  if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(panel, true);
  const ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                     .duty_resolution = LEDC_TIMER_10_BIT,
                                     .timer_num = LEDC_TIMER_0,
                                     .freq_hz = 5000,
                                     .clk_cfg = LEDC_AUTO_CLK};
  if (err == ESP_OK) {
    err = ledc_timer_config(&timer);
    if (err == ESP_OK) timer_ready = 1;
  }
  const ledc_channel_config_t light = {.gpio_num = 21,
                                       .speed_mode = LEDC_LOW_SPEED_MODE,
                                       .channel = LEDC_CHANNEL_0,
                                       .timer_sel = LEDC_TIMER_0,
                                       .duty = 1023};
  if (err == ESP_OK) {
    err = ledc_channel_config(&light);
    if (err == ESP_OK) light_ready = 1;
  }
  if (err == ESP_OK) {
    transfer = heap_caps_malloc(WIDTH * ROWS * sizeof(*transfer),
                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (transfer == NULL) err = ESP_ERR_NO_MEM;
  }
  if (err != ESP_OK) {
    (void)h2_passport_display_deinit();
    return err == ESP_ERR_NO_MEM ? H2_PAL_ERR_NO_MEMORY : H2_PAL_ERR_IO;
  }
  opened = 1;
  return 0;
}

static int screen_close(void* user) {
  (void)user;
  return h2_passport_display_deinit();
}

static int screen_info(void* user, h2_display_info_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_display_info_t){WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565};
  return 0;
}

static int screen_draw(void* user, const h2_display_rect_t* area,
                       const void* pixels, size_t stride,
                       h2_display_pixel_format_t format) {
  (void)user;
  if (!opened) return H2_PAL_ERR_INVALID_STATE;
  if (area == NULL || pixels == NULL || format != H2_DISPLAY_PIXEL_RGB565 ||
      area->x < 0 || area->y < 0 || area->width <= 0 || area->height <= 0 ||
      area->x >= WIDTH || area->y >= HEIGHT || area->width > WIDTH - area->x ||
      area->height > HEIGHT - area->y || stride < (size_t)area->width * 2u)
    return H2_PAL_ERR_INVALID_ARG;
  for (int y = 0; y < area->height; y += ROWS) {
    int rows = area->height - y < ROWS ? area->height - y : ROWS;
    for (int row = 0; row < rows; ++row)
      memcpy(transfer + row * area->width,
             (const uint8_t*)pixels + (size_t)(y + row) * stride,
             (size_t)area->width * sizeof(*transfer));
    if (esp_lcd_panel_draw_bitmap(panel, area->x, area->y + y,
                                  area->x + area->width, area->y + y + rows,
                                  transfer) != ESP_OK ||
        esp_lcd_panel_io_tx_param(panel_io, -1, NULL, 0) != ESP_OK)
      return H2_PAL_ERR_IO;
  }
  return 0;
}

static int screen_present(void* user) {
  (void)user;
  if (!opened) return H2_PAL_ERR_INVALID_STATE;
  return esp_lcd_panel_io_tx_param(panel_io, -1, NULL, 0) == ESP_OK
             ? 0
             : H2_PAL_ERR_IO;
}

static int brightness(void* user, uint32_t percent) {
  (void)user;
  if (percent > 100) return H2_PAL_ERR_INVALID_ARG;
  if (!opened) return H2_PAL_ERR_INVALID_STATE;
  esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                                percent * 1023u / 100u);
  if (err == ESP_OK)
    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  return err == ESP_OK ? 0 : H2_PAL_ERR_IO;
}

const h2_pal_display_api_t* h2_passport_display(void) {
  static const h2_pal_display_vtable_t vtable = {
      .open = screen_open,
      .close = screen_close,
      .get_info = screen_info,
      .draw_bitmap = screen_draw,
      .present = screen_present,
      .set_brightness_percent = brightness};
  static const h2_pal_display_api_t api = {.vtable = &vtable};
  return &api;
}
