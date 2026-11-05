#include "h2_esp_board.h"
#include "h2_esp_board_private.h"
#include "h2_esp_tiga_board_internal.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "soc/spi_struct.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LCD_HOST SPI2_HOST
#define LCD_WIDTH 240
#define LCD_HEIGHT 240
#define LCD_CS_GPIO GPIO_NUM_40
#define LCD_SCLK_GPIO GPIO_NUM_41
#define LCD_MOSI_GPIO GPIO_NUM_42
#define LCD_DC_GPIO GPIO_NUM_39
#define LCD_RST_GPIO GPIO_NUM_45
#define LCD_BL_GPIO GPIO_NUM_46
#define LCD_PIXEL_CLK_HZ (40 * 1000 * 1000)
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LCD_BITS_PER_PIXEL 16
#define LCD_DMA_BUFFER_LINES 16
#define LCD_DMA_BUFFER_PIXELS (LCD_WIDTH * LCD_DMA_BUFFER_LINES)
#define LCD_DMA_BUFFER_BYTES (LCD_DMA_BUFFER_PIXELS * 2u)
#define LCD_COMMAND_PARAM_BYTES 8u
#define LCD_SPI_MAX_TRANSFER_BYTES LCD_DMA_BUFFER_BYTES
#define LCD_BACKLIGHT_TIMER LEDC_TIMER_0
#define LCD_BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define LCD_BACKLIGHT_DUTY_RES LEDC_TIMER_13_BIT
#define LCD_BACKLIGHT_DUTY_MAX ((1u << 13) - 1u)

#define ST7789_CMD_SLEEP_IN 0x10
#define ST7789_CMD_SLEEP_OUT 0x11
#define ST7789_CMD_INVERSION_ON 0x21
#define ST7789_CMD_DISPLAY_OFF 0x28
#define ST7789_CMD_DISPLAY_ON 0x29
#define ST7789_CMD_COLUMN_ADDRESS_SET 0x2a
#define ST7789_CMD_ROW_ADDRESS_SET 0x2b
#define ST7789_CMD_MEMORY_WRITE 0x2c
#define ST7789_CMD_MEMORY_ACCESS_CONTROL 0x36
#define ST7789_CMD_PIXEL_FORMAT_SET 0x3a
#define ST7789_CMD_RAM_CONTROL 0xb0

typedef struct h2_esp_tiga_display_state {
  esp_lcd_panel_io_handle_t panel_io;
  uint8_t* dma_buffer;
  size_t dma_buffer_bytes;
  bool initialized;
  bool opened;
} h2_esp_tiga_display_state_t;

static const char* TAG = "h2_esp_tiga_esp_v4_2";
static uint8_t s_lcd_dma_buffer[LCD_DMA_BUFFER_BYTES]
    __attribute__((aligned(64)));
static uint8_t s_lcd_command_params[LCD_COMMAND_PARAM_BYTES]
    __attribute__((aligned(4)));
static h2_esp_tiga_display_state_t s_display_state = {
    .dma_buffer = s_lcd_dma_buffer,
    .dma_buffer_bytes = sizeof(s_lcd_dma_buffer),
};

static h2_esp_board_display_transfer_capture_fn s_transfer_capture;
static void* s_transfer_capture_user;

h2_display_info_t h2_esp_board_display_set_transfer_capture(
    h2_esp_board_display_transfer_capture_fn capture, void* user) {
  s_transfer_capture = capture;
  s_transfer_capture_user = user;
  return (h2_display_info_t){.width = LCD_WIDTH,
                             .height = LCD_HEIGHT,
                             .native_format = H2_DISPLAY_PIXEL_RGB565};
}

static int esp_result(esp_err_t err) {
  if (err == ESP_OK) {
    return H2_DISPLAY_OK;
  }
  if (err == ESP_ERR_NO_MEM) {
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_STATE) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  return H2_DISPLAY_ERR_IO;
}

static size_t rgb565_byte_count(size_t pixels) { return pixels * 2u; }

static int send_command(h2_esp_tiga_display_state_t* state, int command,
                        const uint8_t* data, size_t len) {
  if (len > sizeof(s_lcd_command_params) || (len > 0u && data == NULL)) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  const uint8_t* tx_data = NULL;
  if (len > 0u) {
    memcpy(s_lcd_command_params, data, len);
    tx_data = s_lcd_command_params;
  }
  esp_err_t err =
      esp_lcd_panel_io_tx_param(state->panel_io, command, tx_data, len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "panel command failed cmd=0x%02x err=%s", command,
             esp_err_to_name(err));
  }
  return esp_result(err);
}

/* Color DMA progress for stall reports: each queued block must be followed by
 * one completion callback before the next panel command can take the bus. */
/* Share the 64-bit timestamps safely with the 32-bit ISR. Critical sections
 * only copy counters/timestamps; formatting and SPI operations stay outside. */
static DRAM_ATTR portMUX_TYPE s_color_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR uint32_t s_color_queued;
static DRAM_ATTR uint32_t s_color_done;
static DRAM_ATTR int64_t s_color_queued_us;
static DRAM_ATTR int64_t s_color_done_us;

static IRAM_ATTR bool color_trans_done(esp_lcd_panel_io_handle_t io,
                                       esp_lcd_panel_io_event_data_t* event,
                                       void* user) {
  (void)io;
  (void)event;
  (void)user;
  const int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&s_color_lock);
  s_color_done_us = now_us;
  ++s_color_done;
  portEXIT_CRITICAL_ISR(&s_color_lock);
  return false;
}

size_t h2_esp_board_display_diagnostics(char* out, size_t capacity) {
  if (out == NULL || capacity == 0u) return 0u;
  portENTER_CRITICAL(&s_color_lock);
  const uint32_t queued = s_color_queued;
  const uint32_t done = s_color_done;
  const int64_t queued_us = s_color_queued_us;
  const int64_t done_us = s_color_done_us;
  portEXIT_CRITICAL(&s_color_lock);
  const int64_t now_us = esp_timer_get_time();
  const int written =
      snprintf(out, capacity,
               "display color_queued=%lu color_done=%lu queued_age_ms=%lld "
               "done_age_ms=%lld spi2_usr=%u spi2_dma_int_raw=0x%08lx "
               "spi2_dma_int_ena=0x%08lx spi2_dma_int_st=0x%08lx\n",
               (unsigned long)queued, (unsigned long)done,
               queued != 0u ? (long long)((now_us - queued_us) / 1000) : -1LL,
               done != 0u ? (long long)((now_us - done_us) / 1000) : -1LL,
               (unsigned)GPSPI2.cmd.usr, (unsigned long)GPSPI2.dma_int_raw.val,
               (unsigned long)GPSPI2.dma_int_ena.val,
               (unsigned long)GPSPI2.dma_int_st.val);
  if (written < 0) return 0u;
  return (size_t)written < capacity ? (size_t)written : capacity - 1u;
}

static int send_color_command(h2_esp_tiga_display_state_t* state, int command,
                              const uint8_t* data, size_t len) {
  esp_err_t err =
      esp_lcd_panel_io_tx_color(state->panel_io, command, data, len);
  if (err == ESP_OK) {
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_color_lock);
    s_color_queued_us = now_us;
    ++s_color_queued;
    portEXIT_CRITICAL(&s_color_lock);
    err = esp_lcd_panel_io_tx_param(state->panel_io, -1, NULL, 0);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "panel color command failed cmd=0x%02x err=%s", command,
             esp_err_to_name(err));
  }
  return esp_result(err);
}

static void encode_range(uint8_t out[4], uint16_t start, uint16_t end) {
  out[0] = (uint8_t)(start >> 8);
  out[1] = (uint8_t)(start & 0xffu);
  out[2] = (uint8_t)(end >> 8);
  out[3] = (uint8_t)(end & 0xffu);
}

static int set_address_window(h2_esp_tiga_display_state_t* state,
                              const h2_display_rect_t* rect) {
  uint8_t column[4];
  uint8_t row[4];
  encode_range(column, (uint16_t)rect->x,
               (uint16_t)(rect->x + rect->width - 1));
  encode_range(row, (uint16_t)rect->y, (uint16_t)(rect->y + rect->height - 1));

  int rc = send_command(state, ST7789_CMD_COLUMN_ADDRESS_SET, column,
                        sizeof(column));
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  return send_command(state, ST7789_CMD_ROW_ADDRESS_SET, row, sizeof(row));
}

static int reset_panel(void) {
  const gpio_config_t reset_cfg = {
      .pin_bit_mask = 1ULL << LCD_RST_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&reset_cfg);
  if (err == ESP_OK) {
    err = gpio_set_level(LCD_RST_GPIO, 1);
  }
  if (err == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(10));
    err = gpio_set_level(LCD_RST_GPIO, 0);
  }
  if (err == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(20));
    err = gpio_set_level(LCD_RST_GPIO, 1);
  }
  if (err == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  return esp_result(err);
}

static int reset_spi_gpios(void) {
  const gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << LCD_CS_GPIO) | (1ULL << LCD_SCLK_GPIO) |
                      (1ULL << LCD_MOSI_GPIO) | (1ULL << LCD_DC_GPIO),
      .mode = GPIO_MODE_DISABLE,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  return esp_result(gpio_config(&cfg));
}

static int set_backlight_percent(uint32_t percent) {
  if (percent > 100u) {
    percent = 100u;
  }
  const uint32_t duty = (percent * LCD_BACKLIGHT_DUTY_MAX) / 100u;
  esp_err_t err =
      ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL, duty);
  if (err == ESP_OK) {
    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL);
  }
  return esp_result(err);
}

static int init_backlight(void) {
  const ledc_timer_config_t timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LCD_BACKLIGHT_DUTY_RES,
      .timer_num = LCD_BACKLIGHT_TIMER,
      .freq_hz = 5000,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  esp_err_t err = ledc_timer_config(&timer);
  if (err != ESP_OK) {
    return esp_result(err);
  }
  const ledc_channel_config_t channel = {
      .gpio_num = LCD_BL_GPIO,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LCD_BACKLIGHT_CHANNEL,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = LCD_BACKLIGHT_TIMER,
      .duty = 0,
      .hpoint = 0,
      .flags =
          {
              .output_invert = 0,
          },
  };
  err = ledc_channel_config(&channel);
  if (err != ESP_OK) {
    return esp_result(err);
  }
  return set_backlight_percent(0);
}

int h2_esp_tiga_display_validate_fixed_memory(void) {
  h2_esp_tiga_display_state_t* state = &s_display_state;
  if (state == NULL || state->dma_buffer != s_lcd_dma_buffer ||
      state->dma_buffer_bytes != LCD_DMA_BUFFER_BYTES ||
      ((uintptr_t)state->dma_buffer & 63u) != 0u ||
      !esp_ptr_internal(state->dma_buffer) ||
      !esp_ptr_dma_capable(state->dma_buffer) ||
      !esp_ptr_internal(s_lcd_command_params) ||
      !esp_ptr_dma_capable(s_lcd_command_params)) {
    ESP_LOGE(TAG,
             "display fixed dma buffer invalid ptr=%p bytes=%u internal=%d "
             "dma=%d aligned=%d command_internal=%d command_dma=%d",
             state != NULL ? state->dma_buffer : NULL,
             state != NULL ? (unsigned)state->dma_buffer_bytes : 0u,
             state != NULL && esp_ptr_internal(state->dma_buffer),
             state != NULL && esp_ptr_dma_capable(state->dma_buffer),
             state != NULL && (((uintptr_t)state->dma_buffer & 63u) == 0u),
             esp_ptr_internal(s_lcd_command_params),
             esp_ptr_dma_capable(s_lcd_command_params));
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  ESP_LOGI(
      TAG,
      "display fixed dma tile ptr=%p lines=%u bytes=%u internal_free=%u "
      "internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u",
      state->dma_buffer, (unsigned)LCD_DMA_BUFFER_LINES,
      (unsigned)state->dma_buffer_bytes,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                 MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA |
                                                 MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return H2_DISPLAY_OK;
}

static int open_panel(h2_esp_tiga_display_state_t* state) {
  static const uint8_t madctl[] = {0x00};
  static const uint8_t colmod[] = {0x05};
  static const uint8_t ramctrl[] = {0x00, 0xf0};

  int rc = reset_panel();
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  rc = send_command(state, ST7789_CMD_SLEEP_OUT, NULL, 0u);
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  vTaskDelay(pdMS_TO_TICKS(100));
  rc = send_command(state, ST7789_CMD_MEMORY_ACCESS_CONTROL, madctl,
                    sizeof(madctl));
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  rc = send_command(state, ST7789_CMD_PIXEL_FORMAT_SET, colmod, sizeof(colmod));
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  rc = send_command(state, ST7789_CMD_RAM_CONTROL, ramctrl, sizeof(ramctrl));
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  rc = send_command(state, ST7789_CMD_INVERSION_ON, NULL, 0u);
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  return H2_DISPLAY_OK;
}

static int clear_panel_black(h2_esp_tiga_display_state_t* state) {
  memset(state->dma_buffer, 0, state->dma_buffer_bytes);
  h2_display_rect_t tile = {
      .x = 0,
      .y = 0,
      .width = LCD_WIDTH,
      .height = LCD_DMA_BUFFER_LINES,
  };
  for (int row = 0; row < LCD_HEIGHT; row += LCD_DMA_BUFFER_LINES) {
    tile.y = row;
    const int remaining_rows = LCD_HEIGHT - row;
    tile.height = remaining_rows < LCD_DMA_BUFFER_LINES ? remaining_rows
                                                        : LCD_DMA_BUFFER_LINES;
    int rc = set_address_window(state, &tile);
    if (rc != H2_DISPLAY_OK) {
      return rc;
    }
    rc = send_color_command(
        state, ST7789_CMD_MEMORY_WRITE, state->dma_buffer,
        rgb565_byte_count((size_t)tile.width * (size_t)tile.height));
    if (rc != H2_DISPLAY_OK) {
      return rc;
    }
    if (s_transfer_capture != NULL)
      s_transfer_capture(s_transfer_capture_user, &tile,
                         (const uint16_t*)state->dma_buffer);
  }
  return H2_DISPLAY_OK;
}

static int init_display(h2_esp_tiga_display_state_t* state) {
  if (state->initialized) {
    return H2_DISPLAY_OK;
  }

  ESP_LOGI(TAG, "init %s ST7789 SPI RGB565 panel", H2_ESP_BOARD_NAME);
  int rc = h2_esp_tiga_board_enable_peripheral_power();
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  rc = init_backlight();
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  const uint32_t bl_off_ms = esp_log_timestamp();
  rc = reset_spi_gpios();
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }

  const spi_bus_config_t buscfg = {
      .sclk_io_num = LCD_SCLK_GPIO,
      .mosi_io_num = LCD_MOSI_GPIO,
      .miso_io_num = GPIO_NUM_NC,
      .quadwp_io_num = GPIO_NUM_NC,
      .quadhd_io_num = GPIO_NUM_NC,
      .max_transfer_sz = LCD_SPI_MAX_TRANSFER_BYTES,
  };
  esp_err_t err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return esp_result(err);
  }

  rc = h2_esp_tiga_display_validate_fixed_memory();
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }

  const esp_lcd_panel_io_spi_config_t io_config = {
      .dc_gpio_num = LCD_DC_GPIO,
      .cs_gpio_num = LCD_CS_GPIO,
      .pclk_hz = LCD_PIXEL_CLK_HZ,
      .lcd_cmd_bits = LCD_CMD_BITS,
      .lcd_param_bits = LCD_PARAM_BITS,
      .spi_mode = 0,
      .trans_queue_depth = 2,
      .on_color_trans_done = color_trans_done,
  };
  err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config,
                                 &state->panel_io);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
    return esp_result(err);
  }

  rc = open_panel(state);
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  rc = clear_panel_black(state);
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  rc = send_command(state, ST7789_CMD_DISPLAY_ON, NULL, 0u);
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  const uint32_t disp_on_ms = esp_log_timestamp();
  rc = set_backlight_percent(100u);
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  const uint32_t bl_on_ms = esp_log_timestamp();
  printf(
      "H2_DISPLAY_BRINGUP mode=cold bl_off_ms=%u disp_on_ms=%u bl_on_ms=%u\n",
      (unsigned)bl_off_ms, (unsigned)disp_on_ms, (unsigned)bl_on_ms);

  state->initialized = true;
  return H2_DISPLAY_OK;
}

static int clip_rect(const h2_display_rect_t* rect,
                     h2_display_rect_t* clipped) {
  int x1 = rect->x;
  int y1 = rect->y;
  int x2 = rect->x + rect->width;
  int y2 = rect->y + rect->height;

  if (x1 < 0) {
    x1 = 0;
  }
  if (y1 < 0) {
    y1 = 0;
  }
  if (x2 > LCD_WIDTH) {
    x2 = LCD_WIDTH;
  }
  if (y2 > LCD_HEIGHT) {
    y2 = LCD_HEIGHT;
  }
  if (x1 >= x2 || y1 >= y2) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }

  clipped->x = x1;
  clipped->y = y1;
  clipped->width = x2 - x1;
  clipped->height = y2 - y1;
  return H2_DISPLAY_OK;
}

static int source_pixel_size(h2_display_pixel_format_t format,
                             size_t* pixel_size) {
  if (pixel_size == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  if (format == H2_DISPLAY_PIXEL_RGB565 || format == H2_DISPLAY_PIXEL_RGB444) {
    *pixel_size = 2u;
    return H2_DISPLAY_OK;
  }
  if (format == H2_DISPLAY_PIXEL_RGB888) {
    *pixel_size = 3u;
    return H2_DISPLAY_OK;
  }
  return H2_DISPLAY_ERR_UNSUPPORTED;
}

static int validate_source_bitmap(const h2_display_rect_t* src_rect,
                                  const void* pixels, size_t stride_bytes,
                                  h2_display_pixel_format_t format,
                                  size_t* src_pixel_size) {
  if (src_rect == NULL || pixels == NULL || src_pixel_size == NULL ||
      src_rect->width <= 0 || src_rect->height <= 0) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  int rc = source_pixel_size(format, src_pixel_size);
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  if ((size_t)src_rect->width > SIZE_MAX / *src_pixel_size) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  const size_t row_bytes = (size_t)src_rect->width * *src_pixel_size;
  if (stride_bytes < row_bytes) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  if ((size_t)(src_rect->height - 1) > (SIZE_MAX - row_bytes) / stride_bytes) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  return H2_DISPLAY_OK;
}

/* PAL RGB565 words use host byte order; the panel consumes MSB first. */
static void convert_bitmap_to_rgb565(const h2_display_rect_t* src_rect,
                                     const h2_display_rect_t* clipped,
                                     const void* pixels, size_t stride_bytes,
                                     h2_display_pixel_format_t format,
                                     size_t src_pixel_size, uint8_t* out) {
  const uint8_t* src = (const uint8_t*)pixels;
  src += (size_t)(clipped->y - src_rect->y) * stride_bytes;
  src += (size_t)(clipped->x - src_rect->x) * src_pixel_size;
  for (int row = 0; row < clipped->height; ++row) {
    const uint8_t* src_row = src + (size_t)row * stride_bytes;
    for (int col = 0; col < clipped->width; ++col) {
      const uint8_t* pixel = src_row + (size_t)col * src_pixel_size;
      uint16_t value;
      if (format == H2_DISPLAY_PIXEL_RGB888) {
        value = (uint16_t)(((uint16_t)(pixel[0] >> 3) << 11) |
                           ((uint16_t)(pixel[1] >> 2) << 5) | (pixel[2] >> 3));
      } else {
        memcpy(&value, pixel, sizeof(value));
        if (format == H2_DISPLAY_PIXEL_RGB444) {
          const uint16_t r = (value >> 8) & 15u;
          const uint16_t g = (value >> 4) & 15u;
          const uint16_t b = value & 15u;
          value = (uint16_t)(((r << 1 | r >> 3) << 11) |
                             ((g << 2 | g >> 2) << 5) | (b << 1 | b >> 3));
        }
      }
      *out++ = (uint8_t)(value >> 8);
      *out++ = (uint8_t)value;
    }
  }
}

static int tiga_get_info(void* user, h2_display_info_t* info) {
  h2_esp_tiga_display_state_t* state = (h2_esp_tiga_display_state_t*)user;
  if (!state->opened) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }

  info->width = LCD_WIDTH;
  info->height = LCD_HEIGHT;
  info->native_format = H2_DISPLAY_PIXEL_RGB565;
  return H2_DISPLAY_OK;
}

static int tiga_draw_bitmap(void* user, const h2_display_rect_t* rect,
                            const void* pixels, size_t stride_bytes,
                            h2_display_pixel_format_t format) {
  h2_esp_tiga_display_state_t* state = (h2_esp_tiga_display_state_t*)user;
  if (!state->opened) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  /* clip_rect() dereferences rect immediately; the vtable entry must reject
   * an invalid rectangle itself instead of relying on the PAL wrapper. */
  if (rect == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }

  h2_display_rect_t clipped;
  int rc = clip_rect(rect, &clipped);
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }

  size_t src_pixel_size = 0u;
  rc = validate_source_bitmap(rect, pixels, stride_bytes, format,
                              &src_pixel_size);
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  if (state->dma_buffer == NULL ||
      state->dma_buffer_bytes < LCD_DMA_BUFFER_BYTES) {
    return H2_DISPLAY_ERR_NO_MEMORY;
  }

  h2_display_rect_t tile = clipped;
  for (int row = 0; row < clipped.height; row += LCD_DMA_BUFFER_LINES) {
    const int remaining_rows = clipped.height - row;
    tile.y = clipped.y + row;
    tile.height = remaining_rows < LCD_DMA_BUFFER_LINES ? remaining_rows
                                                        : LCD_DMA_BUFFER_LINES;
    const size_t tile_pixels = (size_t)tile.width * (size_t)tile.height;
    const size_t encoded_bytes = rgb565_byte_count(tile_pixels);
    convert_bitmap_to_rgb565(rect, &tile, pixels, stride_bytes, format,
                             src_pixel_size, state->dma_buffer);
    rc = set_address_window(state, &tile);
    if (rc != H2_DISPLAY_OK) {
      return rc;
    }
    rc = send_color_command(state, ST7789_CMD_MEMORY_WRITE, state->dma_buffer,
                            encoded_bytes);
    if (rc != H2_DISPLAY_OK) {
      return rc;
    }
    if (s_transfer_capture != NULL)
      s_transfer_capture(s_transfer_capture_user, &tile,
                         (const uint16_t*)state->dma_buffer);
  }
  return H2_DISPLAY_OK;
}

static int tiga_present(void* user) {
  h2_esp_tiga_display_state_t* state = (h2_esp_tiga_display_state_t*)user;
  return state->opened ? H2_DISPLAY_OK : H2_DISPLAY_ERR_INVALID_STATE;
}

static int tiga_set_brightness_percent(void* user, uint32_t percent) {
  h2_esp_tiga_display_state_t* state = (h2_esp_tiga_display_state_t*)user;
  if (!state->opened) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  if (percent > 100u) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  return set_backlight_percent(percent);
}

static int tiga_open(void* user) {
  h2_esp_tiga_display_state_t* state = (h2_esp_tiga_display_state_t*)user;
  if (state->opened) {
    return H2_DISPLAY_OK;
  }

  const bool was_initialized = state->initialized;
  int rc = init_display(state);
  if (rc != H2_DISPLAY_OK) {
    return rc;
  }
  if (was_initialized) {
    rc = send_command(state, ST7789_CMD_SLEEP_OUT, NULL, 0u);
    if (rc == H2_DISPLAY_OK) {
      vTaskDelay(pdMS_TO_TICKS(120u));
      rc = clear_panel_black(state);
    }
    if (rc == H2_DISPLAY_OK) {
      rc = send_command(state, ST7789_CMD_DISPLAY_ON, NULL, 0u);
    }
    const uint32_t disp_on_ms = esp_log_timestamp();
    if (rc == H2_DISPLAY_OK) {
      rc = set_backlight_percent(100u);
    }
    if (rc != H2_DISPLAY_OK) {
      return rc;
    }
    const uint32_t bl_on_ms = esp_log_timestamp();
    printf("H2_DISPLAY_BRINGUP mode=reopen disp_on_ms=%u bl_on_ms=%u\n",
           (unsigned)disp_on_ms, (unsigned)bl_on_ms);
  }
  state->opened = true;
  return H2_DISPLAY_OK;
}

static int tiga_close(void* user) {
  h2_esp_tiga_display_state_t* state = (h2_esp_tiga_display_state_t*)user;
  if (!state->opened || state->panel_io == NULL) {
    return H2_DISPLAY_OK;
  }
  int rc = send_command(state, ST7789_CMD_DISPLAY_OFF, NULL, 0u);
  if (rc == H2_DISPLAY_OK) {
    rc = send_command(state, ST7789_CMD_SLEEP_IN, NULL, 0u);
  }
  if (rc == H2_DISPLAY_OK) {
    vTaskDelay(pdMS_TO_TICKS(120u));
  }
  int off_rc = set_backlight_percent(0u);
  if (rc == H2_DISPLAY_OK) {
    rc = off_rc;
  }
  if (rc == H2_DISPLAY_OK) {
    state->opened = false;
  }
  return rc;
}

h2_pal_display_t* h2_esp_board_display(void) {
  static const h2_pal_display_vtable_t vtable = {
      .open = tiga_open,
      .get_info = tiga_get_info,
      .draw_bitmap = tiga_draw_bitmap,
      .present = tiga_present,
      .set_brightness_percent = tiga_set_brightness_percent,
      .close = tiga_close,
  };
  static h2_pal_display_t display = {
      .user = &s_display_state,
      .vtable = &vtable,
  };
  return &display;
}

h2_pal_display_t* h2_esp_board_display_if_initialized(void) {
  if (!s_display_state.initialized) {
    return NULL;
  }
  return h2_esp_board_display();
}
