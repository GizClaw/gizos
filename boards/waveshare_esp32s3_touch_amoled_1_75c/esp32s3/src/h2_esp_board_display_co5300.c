#include "h2_esp_board_private.h"
#include "h2_esp_board_config.h"
#include "h2_esp_board_internal.h"
#include "h2_esp_board_display_buffer.h"
#include "h2_ws175c_display_capture.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <string.h>

#define LCD_HOST SPI2_HOST
#define LCD_DRAW_ROWS 32u
#define LCD_DRAW_ROWS_MIN 8u
#define LCD_DMA_BYTES (H2_WS175C_LCD_WIDTH * LCD_DRAW_ROWS * sizeof(uint16_t))

typedef struct display_state {
  esp_lcd_panel_io_handle_t io;
  esp_lcd_panel_handle_t panel;
  uint16_t* shadow;
  uint16_t* dma;
  size_t dma_pixels;
  bool owns_bus;
  bool opened;
} display_state_t;

static display_state_t s_display;
static h2_ws175c_transfer_capture_fn s_capture;
static void* s_capture_user;

h2_display_info_t h2_ws175c_display_capture_configure(
    h2_ws175c_transfer_capture_fn capture, void* user) {
  s_capture = capture;
  s_capture_user = user;
  return (h2_display_info_t){
      .width = H2_WS175C_LCD_WIDTH,
      .height = H2_WS175C_LCD_HEIGHT,
      .native_format = H2_DISPLAY_PIXEL_RGB565,
  };
}

/* Panel sequence from Waveshare's CC0 board setup_device.c. Keep the panel
 * dark until its first complete black frame has been transferred. */
static const co5300_lcd_init_cmd_t s_init_commands[] = {
    {0xfe, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1c, (uint8_t[]){0xa0}, 1, 0},
    {0xfe, (uint8_t[]){0x00}, 1, 0},
    {0xc4, (uint8_t[]){0x80}, 1, 0},
    {0x3a, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0xff}, 1, 0},
    {0x2a, (uint8_t[]){0x00, 0x06, 0x01, 0xd7}, 4, 0},
    {0x2b, (uint8_t[]){0x00, 0x00, 0x01, 0xd1}, 4, 600},
    {0x11, NULL, 0, 600},
};

static int display_result(esp_err_t err) {
  if (err == ESP_OK) {
    return H2_DISPLAY_OK;
  }
  if (err == ESP_ERR_NO_MEM) {
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  if (err == ESP_ERR_INVALID_ARG) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  return H2_DISPLAY_ERR_IO;
}

static esp_err_t set_brightness(display_state_t* state, uint32_t percent) {
  const uint8_t brightness = (uint8_t)(percent * 255u / 100u);
  return esp_lcd_panel_io_tx_param(state->io, (0x02 << 24) | (0x51 << 8),
                                   &brightness, sizeof(brightness));
}

static int flush_region(display_state_t* state, h2_display_rect_t area) {
  const int32_t bottom = area.y + area.height;
  const size_t rows = (state->dma_pixels / (size_t)area.width) & ~(size_t)1u;
  if (rows == 0u) {
    return H2_DISPLAY_ERR_NO_MEMORY;
  }
  while (area.y < bottom) {
    const size_t remaining = (size_t)(bottom - area.y);
    const size_t count = rows < remaining ? rows : remaining;
    for (size_t row = 0; row < count; ++row) {
      memcpy(state->dma + row * (size_t)area.width,
             state->shadow + ((size_t)area.y + row) * H2_WS175C_LCD_WIDTH +
                 (size_t)area.x,
             (size_t)area.width * sizeof(uint16_t));
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(
        state->panel, area.x, area.y, area.x + area.width,
        area.y + (int32_t)count, state->dma);
    if (err == ESP_OK) {
      /* A parameter transaction drains queued DMA before this buffer is reused.
       */
      err = esp_lcd_panel_io_tx_param(state->io, -1, NULL, 0);
    }
    if (err != ESP_OK) {
      return display_result(err);
    }
    if (s_capture != NULL) {
      const h2_display_rect_t chunk = {
          .x = area.x,
          .y = area.y,
          .width = area.width,
          .height = (int32_t)count,
      };
      s_capture(s_capture_user, &chunk, state->dma);
    }
    area.y += (int32_t)count;
  }
  return H2_DISPLAY_OK;
}

static void release_display(display_state_t* state) {
  if (state->panel != NULL) {
    (void)esp_lcd_panel_del(state->panel);
  }
  if (state->io != NULL) {
    (void)esp_lcd_panel_io_del(state->io);
  }
  if (state->owns_bus) {
    (void)spi_bus_free(LCD_HOST);
  }
  heap_caps_free(state->dma);
  heap_caps_free(state->shadow);
  memset(state, 0, sizeof(*state));
}

static int initialize_display(display_state_t* state) {
  const spi_bus_config_t bus_config = CO5300_PANEL_BUS_QSPI_CONFIG(
      H2_WS175C_LCD_PCLK_GPIO, H2_WS175C_LCD_DATA0_GPIO,
      H2_WS175C_LCD_DATA1_GPIO, H2_WS175C_LCD_DATA2_GPIO,
      H2_WS175C_LCD_DATA3_GPIO, LCD_DMA_BYTES);
  esp_err_t err = spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    return display_result(err);
  }
  state->owns_bus = true;
  state->shadow =
      heap_caps_calloc((size_t)H2_WS175C_LCD_WIDTH * H2_WS175C_LCD_HEIGHT,
                       sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  for (size_t rows = LCD_DRAW_ROWS; rows >= LCD_DRAW_ROWS_MIN; rows /= 2u) {
    state->dma_pixels = H2_WS175C_LCD_WIDTH * rows;
    state->dma = heap_caps_malloc(state->dma_pixels * sizeof(uint16_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (state->dma != NULL) {
      break;
    }
  }
  if (state->shadow == NULL || state->dma == NULL) {
    release_display(state);
    return H2_DISPLAY_ERR_NO_MEMORY;
  }

  const esp_lcd_panel_io_spi_config_t io_config =
      CO5300_PANEL_IO_QSPI_CONFIG(H2_WS175C_LCD_CS_GPIO, NULL, NULL);
  err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config,
                                 &state->io);
  co5300_vendor_config_t vendor = {
      .init_cmds = s_init_commands,
      .init_cmds_size = sizeof(s_init_commands) / sizeof(s_init_commands[0]),
      .flags.use_qspi_interface = true,
  };
  const esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = H2_WS175C_LCD_RESET_GPIO,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
      .bits_per_pixel = 16,
      .vendor_config = &vendor,
  };
  if (err == ESP_OK) {
    err = esp_lcd_new_panel_co5300(state->io, &panel_config, &state->panel);
  }
  if (err == ESP_OK) {
    err = esp_lcd_panel_reset(state->panel);
  }
  if (err == ESP_OK) {
    err = esp_lcd_panel_init(state->panel);
  }
  if (err == ESP_OK) {
    err = esp_lcd_panel_set_gap(state->panel, H2_WS175C_LCD_X_GAP, 0);
  }
  int rc = display_result(err);
  if (rc == H2_DISPLAY_OK) {
    rc = flush_region(state, (h2_display_rect_t){
                                 .width = H2_WS175C_LCD_WIDTH,
                                 .height = H2_WS175C_LCD_HEIGHT,
                             });
  }
  if (rc != H2_DISPLAY_OK) {
    release_display(state);
  }
  return rc;
}

static int display_open(void* user) {
  display_state_t* state = user;
  if (state->opened) {
    return H2_DISPLAY_OK;
  }
  if (state->panel == NULL) {
    int rc = initialize_display(state);
    if (rc != H2_DISPLAY_OK) {
      return rc;
    }
  }
  esp_err_t err = esp_lcd_panel_disp_on_off(state->panel, true);
  if (err == ESP_OK) {
    err = set_brightness(state, 50u);
  }
  state->opened = err == ESP_OK;
  return display_result(err);
}

static int display_get_info(void* user, h2_display_info_t* info) {
  display_state_t* state = user;
  if (info == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  if (!state->opened) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  *info = (h2_display_info_t){
      .width = H2_WS175C_LCD_WIDTH,
      .height = H2_WS175C_LCD_HEIGHT,
      .native_format = H2_DISPLAY_PIXEL_RGB565,
  };
  return H2_DISPLAY_OK;
}

static int display_draw_bitmap(void* user, const h2_display_rect_t* rect,
                               const void* pixels, size_t stride_bytes,
                               h2_display_pixel_format_t format) {
  display_state_t* state = user;
  if (!state->opened) {
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  h2_display_rect_t transfer;
  int rc = h2_ws175c_display_buffer_update(state->shadow, rect, pixels,
                                           stride_bytes, format, &transfer);
  return rc == H2_DISPLAY_OK ? flush_region(state, transfer) : rc;
}

static int display_present(void* user) {
  return ((display_state_t*)user)->opened ? H2_DISPLAY_OK
                                          : H2_DISPLAY_ERR_INVALID_STATE;
}

static int display_brightness(void* user, uint32_t percent) {
  display_state_t* state = user;
  if (percent > 100u) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  return state->opened ? display_result(set_brightness(state, percent))
                       : H2_DISPLAY_ERR_INVALID_STATE;
}

static int display_close(void* user) {
  display_state_t* state = user;
  if (!state->opened) {
    return H2_DISPLAY_OK;
  }
  esp_err_t brightness_err = set_brightness(state, 0u);
  esp_err_t err = esp_lcd_panel_disp_on_off(state->panel, false);
  if (err == ESP_OK && brightness_err == ESP_OK) {
    state->opened = false;
  }
  return display_result(brightness_err != ESP_OK ? brightness_err : err);
}

int h2_esp_board_display_power_off(void) {
  if (s_display.panel == NULL) {
    return H2_DISPLAY_OK;
  }
  int rc = display_close(&s_display);
  if (rc == H2_DISPLAY_OK) {
    rc = display_result(esp_lcd_panel_io_tx_param(
        s_display.io, (0x02 << 24) | (0x10 << 8), NULL, 0));
  }
  vTaskDelay(pdMS_TO_TICKS(120));
  (void)gpio_set_level(H2_WS175C_LCD_RESET_GPIO, 0);
  release_display(&s_display);
  return rc;
}

h2_pal_display_t* h2_esp_board_display(void) {
  static const h2_pal_display_vtable_t vtable = {
      .open = display_open,
      .get_info = display_get_info,
      .draw_bitmap = display_draw_bitmap,
      .present = display_present,
      .set_brightness_percent = display_brightness,
      .close = display_close,
  };
  static h2_pal_display_t display = {.user = &s_display, .vtable = &vtable};
  return &display;
}

h2_pal_display_t* h2_esp_board_display_if_initialized(void) {
  return s_display.panel != NULL ? h2_esp_board_display() : NULL;
}
