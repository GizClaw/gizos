#include "h2_mosaico_display.h"
#include "h2_mosaico_display_capture.h"
#include "h2_mosaico_surface.h"
#ifdef H2_MOSAICO_DISPLAY_HOST_TEST
#include "h2_mosaico_display_test_sdk.h"
#else
#include "bsp/display.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#include <string.h>

/* The upstream BSP owns panel and touch handles for the boot lifetime.
 * PAL sessions own the shadow, DMA block and pointer-edge state, never these
 * handles. */
static esp_lcd_panel_handle_t panel;
static esp_lcd_touch_handle_t touch;
static uint16_t* row;
static uint16_t* surface;
enum { DMA_BANDS = H2_MOSAICO_HEIGHT / H2_MOSAICO_DMA_ROWS };
static uint64_t dirty_bands;
static uint16_t dirty_x0[DMA_BANDS], dirty_x1[DMA_BANDS];
static bool opened;
static bool touch_opened;
static bool pressed;
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
      row = heap_caps_malloc(BSP_LCD_H_RES * H2_MOSAICO_DMA_ROWS * sizeof(*row),
                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
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
  int first = 0, end = 0;
  if (rc == H2_PAL_OK)
    rc = h2_mosaico_surface_blit(surface, rect, pixels, stride, &first, &end);
  if (rc == H2_PAL_OK) {
    /* CO5300 requires four-column windows and the verified eight-row
     * transfers. Keep neighboring pixels in the shadow, and merge draws
     * until present instead of transmitting the same band repeatedly. */
    const int x0 = rect->x & ~3;
    const int x1 = (rect->x + rect->width + 3) & ~3;
    for (int band = first / H2_MOSAICO_DMA_ROWS;
         band < end / H2_MOSAICO_DMA_ROWS; ++band) {
      const uint64_t bit = UINT64_C(1) << band;
      if (!(dirty_bands & bit)) {
        dirty_x0[band] = (uint16_t)x0;
        dirty_x1[band] = (uint16_t)x1;
      } else {
        if (x0 < dirty_x0[band]) dirty_x0[band] = (uint16_t)x0;
        if (x1 > dirty_x1[band]) dirty_x1[band] = (uint16_t)x1;
      }
      dirty_bands |= bit;
    }
  }
  xSemaphoreGive(mutex);
  return rc;
}
/* Called with the display lock. DMA borrows row until drain completes;
 * failed bands stay dirty so a later present can retry their latest pixels. */
static int flush_pending(void) {
  if (dirty_bands == 0u) return H2_PAL_OK;
  const int pending_rc = drain();
  if (pending_rc != H2_PAL_OK) return pending_rc;
  for (int band = 0; band < DMA_BANDS; ++band) {
    const uint64_t bit = UINT64_C(1) << band;
    if (!(dirty_bands & bit)) continue;
    const int x0 = dirty_x0[band], x1 = dirty_x1[band];
    const int y = band * H2_MOSAICO_DMA_ROWS;
    const size_t width = (size_t)(x1 - x0);
    for (int r = 0; r < H2_MOSAICO_DMA_ROWS; ++r)
      memcpy(row + (size_t)r * width,
             surface + (size_t)(y + r) * BSP_LCD_H_RES + (size_t)x0,
             width * sizeof(*row));
    int rc = result(esp_lcd_panel_draw_bitmap(panel, x0, y, x1,
                                              y + H2_MOSAICO_DMA_ROWS, row));
    const int drained = drain();
    if (rc == H2_PAL_OK) rc = drained;
    if (rc != H2_PAL_OK) return rc;
    dirty_bands &= ~bit;
    if (transfer_capture) {
      const h2_display_rect_t chunk = {x0, y, (int)width, H2_MOSAICO_DMA_ROWS};
      transfer_capture(transfer_capture_user, &chunk, row);
    }
  }
  return H2_PAL_OK;
}
static int display_present(void* user) {
  (void)user;
  int rc = lock();
  if (rc != H2_PAL_OK) return rc;
  rc = opened ? flush_pending() : H2_PAL_ERR_INVALID_STATE;
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
      heap_caps_free(surface);
      surface = NULL;
      dirty_bands = 0u;
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
  if (!touch_opened)
    rc = H2_PAL_ERR_INVALID_STATE;
  else {
    /* HYN212 may return only the ACK for the previous report. No new
     * snapshot means no event, not an I/O error or a pointer release. */
    const esp_err_t touch_rc = esp_lcd_touch_read_data(touch);
    rc = touch_rc == ESP_ERR_NOT_FINISHED ? H2_PAL_ERR_WOULD_BLOCK
                                          : result(touch_rc);
  }
  if (rc == H2_PAL_OK) {
    uint16_t x = 0, y = 0;
    uint8_t count = 0;
    const bool down =
        esp_lcd_touch_get_coordinates(touch, &x, &y, NULL, &count, 1) && count;
    if (down && (x >= BSP_LCD_H_RES || y >= BSP_LCD_V_RES))
      rc = H2_PAL_ERR_IO;
    else if (down && (!pressed || x != last_x || y != last_y)) {
      *out = (h2_pal_touch_event_t){
          pressed ? H2_PAL_TOUCH_EVENT_MOVE : H2_PAL_TOUCH_EVENT_DOWN, x, y};
      pressed = true;
      last_x = x;
      last_y = y;
    } else if (!down && pressed) {
      *out = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, last_x, last_y};
      pressed = false;
    } else
      rc = H2_PAL_ERR_WOULD_BLOCK;
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
