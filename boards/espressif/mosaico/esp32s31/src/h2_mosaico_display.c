#include "h2_mosaico_display.h"
#include "h2_mosaico_display_capture.h"
#include "h2_mosaico_surface.h"
#include "bsp/display.h"
#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

/* The upstream BSP owns panel and touch handles for the boot lifetime.
 * PAL sessions own the shadow, DMA block and pointer-edge state, never these
 * handles. */
static esp_lcd_panel_handle_t panel;
static esp_lcd_touch_handle_t touch;
/* Keep the panel's eight-row alignment while grouping four chunks into
 * one address window. The pinned SPI IO splits it into bus-sized DMA jobs. */
enum { H2_MOSAICO_DMA_BATCH_ROWS = H2_MOSAICO_DMA_ROWS * 4 };
static uint16_t* row;
static size_t dma_pixels;
static uint16_t* surface;
static bool opened;
static bool touch_opened;
static bool pressed;
static bool touch_retry;
static int64_t touch_contact_us;
enum { H2_MOSAICO_TOUCH_LOST_US = 500000 };
static uint16_t last_x, last_y;
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t mutex;
static h2_mosaico_transfer_capture_fn transfer_capture;
static void* transfer_capture_user;

void h2_mosaico_display_set_transfer_capture(
    h2_mosaico_transfer_capture_fn capture, void* user) {
  transfer_capture = capture;
  transfer_capture_user = user;
}

int h2_mosaico_display_init(void) {
  if (!mutex) mutex = xSemaphoreCreateMutexStatic(&lock_storage);
  return mutex ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
}

static int lock(void) {
  if (!mutex) return H2_PAL_ERR_INVALID_STATE;
  return xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) == pdTRUE
             ? H2_PAL_OK
             : H2_PAL_ERR_TIMEOUT;
}
static int result(esp_err_t rc) {
  if (rc == ESP_OK) return H2_PAL_OK;
  if (rc == ESP_ERR_NO_MEM) return H2_PAL_ERR_NO_MEMORY;
  if (rc == ESP_ERR_TIMEOUT) return H2_PAL_ERR_TIMEOUT;
  return H2_PAL_ERR_IO;
}
static int drain(void) {
  return result(
      esp_lcd_panel_io_tx_param(bsp_display_get_panel_io(), -1, NULL, 0));
}
static int display_open(void* user) {
  (void)user;
  int rc = lock();
  if (rc != H2_PAL_OK) return rc;
  if (!opened) {
    const bsp_display_config_t config = BSP_DISPLAY_DEFAULT_CONFIG();
    rc = result(bsp_display_new(&config, &panel));
    if (rc == H2_PAL_OK && !row) {
      for (int rows = H2_MOSAICO_DMA_BATCH_ROWS;
           row == NULL && rows >= H2_MOSAICO_DMA_ROWS; rows /= 2) {
        const size_t pixels = (size_t)BSP_LCD_H_RES * (size_t)rows;
        row = heap_caps_malloc(pixels * sizeof(*row),
                               MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (row != NULL) dma_pixels = pixels;
      }
      if (!row) rc = H2_PAL_ERR_NO_MEMORY;
    }
    if (rc == H2_PAL_OK && !surface) {
      surface =
          heap_caps_calloc(BSP_LCD_H_RES * BSP_LCD_V_RES, sizeof(*surface),
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!surface) rc = H2_PAL_ERR_NO_MEMORY;
    }
    if (rc == H2_PAL_OK) rc = result(bsp_display_on());
    if (rc == H2_PAL_OK)
      opened = true;
    else {
      heap_caps_free(row);
      row = NULL;
      dma_pixels = 0u;
      heap_caps_free(surface);
      surface = NULL;
    }
  }
  xSemaphoreGive(mutex);
  return rc;
}
static int display_info(void* user, h2_display_info_t* out) {
  (void)user;
  if (!out) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  int rc = lock();
  if (rc != H2_PAL_OK) return rc;
  if (!opened)
    rc = H2_PAL_ERR_INVALID_STATE;
  else
    *out = (h2_display_info_t){BSP_LCD_H_RES, BSP_LCD_V_RES,
                               H2_DISPLAY_PIXEL_RGB565};
  xSemaphoreGive(mutex);
  return rc;
}
static int display_draw(void* user, const h2_display_rect_t* rect,
                        const void* pixels, size_t stride,
                        h2_display_pixel_format_t format) {
  (void)user;
  if (!rect || !pixels || rect->x < 0 || rect->y < 0 || rect->width <= 0 ||
      rect->height <= 0 || rect->width > BSP_LCD_H_RES ||
      rect->height > BSP_LCD_V_RES || rect->x > BSP_LCD_H_RES - rect->width ||
      rect->y > BSP_LCD_V_RES - rect->height)
    return H2_PAL_ERR_INVALID_ARG;
  if (format != H2_DISPLAY_PIXEL_RGB565) return H2_PAL_ERR_UNSUPPORTED;
  const size_t bytes = (size_t)rect->width * 2u;
  if (stride < bytes ||
      (rect->height > 1 &&
       stride > (SIZE_MAX - bytes) / (size_t)(rect->height - 1)))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = lock();
  if (rc != H2_PAL_OK) return rc;
  if (!opened) rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) rc = drain();
  h2_display_rect_t dirty = {0};
  if (rc == H2_PAL_OK)
    rc = h2_mosaico_surface_blit(surface, rect, pixels, stride, &dirty);
  /* Keep the qualified four-column/eight-row panel alignment, packing only
   * the dirty span. Narrow regions can use more rows in the same DMA block. */
  const size_t rows_per_batch =
      rc == H2_PAL_OK ? (dma_pixels / (size_t)dirty.width) /
                            H2_MOSAICO_DMA_ROWS * H2_MOSAICO_DMA_ROWS
                      : 0u;
  if (rc == H2_PAL_OK && rows_per_batch == 0u) rc = H2_PAL_ERR_NO_MEMORY;
  const int bottom = dirty.y + dirty.height;
  for (int y = dirty.y; rc == H2_PAL_OK && y < bottom;) {
    const int rows = (size_t)(bottom - y) < rows_per_batch
                         ? bottom - y
                         : (int)rows_per_batch;
    for (int r = 0; r < rows; ++r)
      memcpy(row + (size_t)r * (size_t)dirty.width,
             surface + (size_t)(y + r) * BSP_LCD_H_RES + (size_t)dirty.x,
             (size_t)dirty.width * sizeof(*row));
    rc = result(esp_lcd_panel_draw_bitmap(
        panel, dirty.x, y, dirty.x + dirty.width, y + rows, row));
    const int drained = drain();
    if (rc == H2_PAL_OK) rc = drained;
    if (rc == H2_PAL_OK && transfer_capture) {
      const h2_display_rect_t chunk = {dirty.x, y, dirty.width, rows};
      transfer_capture(transfer_capture_user, &chunk, row);
    }
    y += rows;
  }
  xSemaphoreGive(mutex);
  return rc;
}
static int display_present(void* user) {
  (void)user;
  int rc = lock();
  if (rc != H2_PAL_OK) return rc;
  rc = opened ? drain() : H2_PAL_ERR_INVALID_STATE;
  xSemaphoreGive(mutex);
  return rc;
}
static int display_brightness(void* user, uint32_t percent) {
  (void)user;
  if (percent > 100) return H2_PAL_ERR_INVALID_ARG;
  int rc = lock();
  if (rc != H2_PAL_OK) return rc;
  rc = opened ? result(bsp_display_brightness_set((int)percent))
              : H2_PAL_ERR_INVALID_STATE;
  xSemaphoreGive(mutex);
  return rc;
}
static int display_close(void* user) {
  (void)user;
  int rc = lock();
  if (rc != H2_PAL_OK) return rc;
  if (opened) {
    rc = drain();
    if (rc == H2_PAL_OK) rc = result(bsp_display_off());
    if (rc == H2_PAL_OK) {
      heap_caps_free(row);
      row = NULL;
      dma_pixels = 0u;
      heap_caps_free(surface);
      surface = NULL;
      opened = false;
    }
  }
  xSemaphoreGive(mutex);
  return rc;
}
static int touch_open(void* user) {
  (void)user;
  int rc = lock();
  if (rc != H2_PAL_OK) return rc;
  if (!touch_opened) {
    rc = result(bsp_touch_new(BSP_LCD_ROTATION_DEFAULT, &touch));
    if (rc == H2_PAL_OK) {
      touch_opened = true;
      pressed = false;
      touch_retry = false;
      touch_contact_us = 0;
    }
  }
  xSemaphoreGive(mutex);
  return rc;
}
static int touch_info(void* user, h2_pal_touch_info_t* out) {
  (void)user;
  if (!out) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  int rc = lock();
  if (rc != H2_PAL_OK) return rc;
  if (!touch_opened)
    rc = H2_PAL_ERR_INVALID_STATE;
  else
    *out = (h2_pal_touch_info_t){BSP_LCD_H_RES, BSP_LCD_V_RES};
  xSemaphoreGive(mutex);
  return rc;
}
static int touch_poll(void* user, h2_pal_touch_event_t* out) {
  (void)user;
  if (!out) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  int rc = lock();
  if (rc != H2_PAL_OK) return rc;
  const int64_t now_us = esp_timer_get_time();
  if (!touch_opened)
    rc = H2_PAL_ERR_INVALID_STATE;
  else if (!pressed && !touch_retry && gpio_get_level(BSP_LCD_TOUCH_INT) != 0)
    rc = H2_PAL_ERR_WOULD_BLOCK;
  else {
    const esp_err_t read_result = esp_lcd_touch_read_data(touch);
    touch_retry = read_result != ESP_OK;
    rc = read_result == ESP_ERR_NOT_FINISHED ? H2_PAL_ERR_WOULD_BLOCK
                                             : result(read_result);
  }
  if (rc == H2_PAL_OK) {
    uint16_t x = 0, y = 0;
    uint8_t count = 0;
    const bool down =
        esp_lcd_touch_get_coordinates(touch, &x, &y, NULL, &count, 1) && count;
    if (down && (x >= BSP_LCD_H_RES || y >= BSP_LCD_V_RES))
      rc = H2_PAL_ERR_IO;
    else if (down) {
      touch_contact_us = now_us;
      if (!pressed || x != last_x || y != last_y) {
        *out = (h2_pal_touch_event_t){
            pressed ? H2_PAL_TOUCH_EVENT_MOVE : H2_PAL_TOUCH_EVENT_DOWN, x, y};
        pressed = true;
        last_x = x;
        last_y = y;
      } else {
        rc = H2_PAL_ERR_WOULD_BLOCK;
      }
    } else if (!down && pressed) {
      *out = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, last_x, last_y};
      pressed = false;
    } else
      rc = H2_PAL_ERR_WOULD_BLOCK;
  }
  if (touch_opened && pressed &&
      now_us - touch_contact_us >= H2_MOSAICO_TOUCH_LOST_US) {
    *out = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, last_x, last_y};
    pressed = false;
    rc = H2_PAL_OK;
  }
  xSemaphoreGive(mutex);
  return rc;
}
static int touch_close(void* user) {
  (void)user;
  int rc = lock();
  if (rc != H2_PAL_OK) return rc;
  touch_opened = false;
  pressed = false;
  touch_retry = false;
  xSemaphoreGive(mutex);
  return H2_PAL_OK;
}
int h2_mosaico_display_deinit(void) {
  if (!mutex) return H2_PAL_OK;
  int rc = touch_close(NULL);
  return rc == H2_PAL_OK ? display_close(NULL) : rc;
}
static const h2_pal_display_vtable_t display_ops = {
    .open = display_open,
    .get_info = display_info,
    .draw_bitmap = display_draw,
    .present = display_present,
    .set_brightness_percent = display_brightness,
    .close = display_close,
};
static const h2_pal_touch_vtable_t touch_ops = {
    .open = touch_open,
    .get_info = touch_info,
    .poll_event = touch_poll,
    .close = touch_close,
};
static const h2_pal_display_api_t display_api = {.vtable = &display_ops};
static const h2_pal_touch_api_t touch_api = {.vtable = &touch_ops};
const h2_pal_display_api_t* h2_mosaico_display(void) { return &display_api; }
const h2_pal_touch_api_t* h2_mosaico_touch(void) { return &touch_api; }
