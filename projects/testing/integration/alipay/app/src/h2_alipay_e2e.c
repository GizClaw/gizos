#include "h2_alipay_e2e.h"
#include "h2_atomic.h"
#include "h2_lvgl_platform.h"
#include "h2_qrcode.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

typedef struct alipay_page {
  h2_runtime_t* runtime;
  const h2_alipay_e2e_config_t* config;
  h2_alipay_e2e_result_t result;
  h2_alipay_e2e_result_t binding_result;
  h2_atomic_bool_t done;
  h2_atomic_bool_t canceled;
  h2_pal_task_t* worker;
  lv_display_t* display;
  lv_obj_t* status;
  lv_obj_t* detail;
  lv_obj_t* qr_canvas;
  uint8_t* pixels;
  uint16_t* qr_pixels;
  size_t stride;
  uint32_t flushes;
  int display_rc;
} alipay_page_t;

static bool canceled(void* user) {
  alipay_page_t* page = user;
  return h2_atomic_bool_load(&page->canceled, H2_ATOMIC_ACQUIRE);
}

static void complete_binding(void* user) {
  alipay_page_t* page = user;
  (void)h2_alipay_e2e_complete_binding(page->runtime, page->config, canceled,
                                       page, &page->binding_result);
  h2_atomic_bool_store(&page->done, true, H2_ATOMIC_RELEASE);
  (void)h2_runtime_notify(page->runtime);
}

static void refresh_binding(void* user) {
  alipay_page_t* page = user;
  (void)h2_alipay_e2e_refresh_binding(page->runtime, page->config, canceled,
                                      page, &page->binding_result);
  h2_atomic_bool_store(&page->done, true, H2_ATOMIC_RELEASE);
  (void)h2_runtime_notify(page->runtime);
}

static void execute(void* user) {
  alipay_page_t* page = user;
  (void)h2_alipay_e2e_execute(page->runtime, page->config, &page->result);
  h2_atomic_bool_store(&page->done, true, H2_ATOMIC_RELEASE);
  (void)h2_runtime_notify(page->runtime);
}

static void flush(lv_display_t* display, const lv_area_t* area,
                  uint8_t* pixels) {
  alipay_page_t* page = lv_display_get_user_data(display);
  const h2_display_rect_t rect = {.x = area->x1,
                                  .y = area->y1,
                                  .width = area->x2 - area->x1 + 1,
                                  .height = area->y2 - area->y1 + 1};
  int rc = h2_pal_display_draw_bitmap(page->runtime->display, &rect, pixels,
                                      page->stride, H2_DISPLAY_PIXEL_RGB565);
  if (rc == H2_PAL_OK) rc = h2_pal_display_present(page->runtime->display);
  if (rc != H2_PAL_OK)
    page->display_rc = rc;
  else
    ++page->flushes;
  lv_display_flush_ready(display);
}

static lv_obj_t* label(const char* text, int x, int y, int width) {
  lv_obj_t* object = lv_label_create(lv_screen_active());
  if (object != NULL) {
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_label_set_text(object, text);
  }
  return object;
}

static int show_result(alipay_page_t* page, int width, int height) {
  h2_alipay_e2e_result_t* r = &page->result;
  char text[192];
  if (r->init_rc == H2_PAL_OK && r->init_sdk_rc == 0)
    lv_label_set_text(page->status, "pre_init: PASS (0)");
  else if (r->init_rc == H2_ALIPAY_E2E_NOT_CALLED)
    lv_label_set_text(page->status, "pre_init: NOT CALLED");
  else
    lv_label_set_text(page->status, "pre_init: FAIL");
  (void)snprintf(text, sizeof(text),
                 "Network / time: %d\nCached reentry: %s\n"
                 "Stage: %d   Result: %d\nSDK return: %d\n%s",
                 r->prepare_rc, r->reused ? "PASS" : "NOT PASSED",
                 (int)r->stage, r->result, r->code_sdk_rc,
                 r->code_sdk_rc == H2_ALIPAY_E2E_NOT_CALLED
                     ? "Binding code: NOT CALLED"
                 : r->result == H2_PAL_OK ? "Binding code: READY"
                                          : "Binding code: FAILED");
  lv_label_set_text(page->detail, text);
  if (r->stage != H2_ALIPAY_E2E_CODE_READY || r->result != H2_PAL_OK)
    return H2_PAL_OK;
  uint8_t modules[H2_QRCODE_BUFFER_LEN_FOR_VERSION(17)];
  uint8_t scratch[sizeof(modules)];
  h2_qrcode_t qr = {0};
  int rc =
      h2_qrcode_encode_text(r->code, H2_QRCODE_ECC_HIGH, 17, modules,
                            sizeof(modules), scratch, sizeof(scratch), &qr);
  if (rc != H2_PAL_OK) return rc;
  h2_qrcode_layout_t layout;
  rc = h2_qrcode_layout_center(&qr, width - 16, height - 52, 4, &layout);
  if (rc != H2_PAL_OK) return rc;
  int span = (qr.size + 8) * layout.scale;
  int canvas_width =
      (int)(lv_draw_buf_width_to_stride(span, LV_COLOR_FORMAT_RGB565) / 2);
  lv_obj_t* canvas = lv_canvas_create(lv_screen_active());
  if (canvas == NULL) return H2_PAL_ERR_NO_MEMORY;
  size_t count = (size_t)canvas_width * span;
  page->qr_pixels = h2_pal_mem_alloc(page->runtime->mem, count * 2u);
  if (page->qr_pixels == NULL) {
    lv_obj_delete(canvas);
    return H2_PAL_ERR_NO_MEMORY;
  }
  layout.origin_x = layout.origin_y = 0;
  rc = h2_qrcode_render_rgb565_band(&qr, &layout, 0, UINT16_MAX, UINT16_MAX, 0,
                                    canvas_width, span, page->qr_pixels, count);
  if (rc == H2_PAL_OK) {
    page->qr_canvas = canvas;
    lv_canvas_set_buffer(canvas, page->qr_pixels, span, span,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas, (width - span) / 2, 28 + (height - 52 - span) / 2);
    lv_obj_set_hidden(page->detail, true);
    lv_obj_set_pos(page->status, 8, height - 20);
    lv_obj_set_width(page->status, width - 16);
    lv_label_set_text(page->status, "pre_init OK / code ready");
  }
  return rc;
}

static int drain(h2_runtime_t* runtime) {
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {.payload = payload,
                              .payload_capacity = sizeof(payload)};
  int rc;
  while ((rc = h2_runtime_poll_event(runtime, &event)) == H2_PAL_OK) {
  }
  return rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT ? H2_PAL_OK
                                                                  : rc;
}

static int join_worker(alipay_page_t* page) {
  int rc = H2_PAL_OK;
  while (!h2_atomic_bool_load(&page->done, H2_ATOMIC_ACQUIRE)) {
    if (page->config->should_exit != NULL &&
        page->config->should_exit(page->config->user)) {
      rc = H2_PAL_ERR_CLOSED;
      break;
    }
    rc = drain(page->runtime);
    if (rc != H2_PAL_OK) break;
    int wait = h2_runtime_wait_notify(page->runtime, 100);
    if (wait != H2_PAL_OK && wait != H2_PAL_ERR_TIMEOUT) {
      rc = wait;
      break;
    }
  }
  if (rc != H2_PAL_OK)
    h2_atomic_bool_store(&page->canceled, true, H2_ATOMIC_RELEASE);
  while (h2_pal_task_join(page->runtime->task, page->worker) != H2_PAL_OK)
    (void)h2_pal_time_sleep_ms(page->runtime->time, 100);
  page->worker = NULL;
  return rc;
}

static void clear_code(alipay_page_t* page) {
  if (page->qr_canvas != NULL) {
    lv_obj_delete(page->qr_canvas);
    page->qr_canvas = NULL;
  }
  h2_pal_mem_free(page->runtime->mem, page->qr_pixels);
  page->qr_pixels = NULL;
  page->result.qr_visible = false;
}

static void present_result(alipay_page_t* page, int width, int height) {
  int render_rc = show_result(page, width, height);
  if (render_rc != H2_PAL_OK)
    lv_label_set_text(page->detail, "QR rendering failed");
  uint32_t prior_flushes = page->flushes;
  lv_refr_now(page->display);
  page->result.display_rc =
      render_rc != H2_PAL_OK ? render_rc : page->display_rc;
  if (page->flushes == prior_flushes && page->result.display_rc == H2_PAL_OK)
    page->result.display_rc = H2_PAL_ERR_IO;
  if (page->result.display_rc == H2_PAL_OK && page->config->observe != NULL)
    page->result.display_rc = page->config->observe(
        page->config->user, page->pixels, page->stride, width, height);
  page->result.qr_visible = page->result.stage == H2_ALIPAY_E2E_CODE_READY &&
                            page->result.result == H2_PAL_OK &&
                            page->result.display_rc == H2_PAL_OK;
  page->result.code_presented = page->result.qr_visible;
}

h2_pal_result_t h2_alipay_e2e_run(h2_runtime_t* runtime,
                                  const h2_alipay_e2e_config_t* config) {
  if (runtime == NULL || config == NULL || config->prepare == NULL ||
      config->alipay == NULL || config->alipay->vtable == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  alipay_page_t page = {.runtime = runtime, .config = config};
  bool platform_open = false, lvgl_open = false, atomic_open = false;
  bool cancel_open = false;
  int rc = h2_pal_display_open(runtime->display);
  if (rc != H2_PAL_OK) return rc;
  h2_display_info_t info = {0};
  rc = h2_pal_display_get_info(runtime->display, &info);
  if (rc != H2_PAL_OK) goto done;
  if (info.width < 240 || info.width > 1024 || info.height < 240 ||
      info.height > 1024 || info.native_format != H2_DISPLAY_PIXEL_RGB565) {
    rc = H2_PAL_ERR_UNSUPPORTED;
    goto done;
  }
  const h2_lvgl_platform_config_t platform = {.allocator = runtime->mem,
                                              .task_api = runtime->task,
                                              .sync_api = runtime->sync,
                                              .queue_api = runtime->queue,
                                              .time_api = runtime->time};
  rc = h2_lvgl_platform_init(&platform);
  if (rc != H2_PAL_OK) goto done;
  platform_open = true;
  lv_init();
  lvgl_open = true;
  page.stride = lv_draw_buf_width_to_stride(info.width, LV_COLOR_FORMAT_RGB565);
  size_t bytes = page.stride * (size_t)info.height;
  page.pixels = h2_pal_mem_alloc(runtime->mem, bytes);
  page.display = lv_display_create(info.width, info.height);
  if (page.pixels == NULL || page.display == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  lv_display_set_default(page.display);
  lv_display_set_user_data(page.display, &page);
  lv_display_set_color_format(page.display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(page.display, flush);
  lv_display_set_buffers(page.display, page.pixels, NULL, (uint32_t)bytes,
                         LV_DISPLAY_RENDER_MODE_FULL);
  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_scrollable(screen, false);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_color(screen, lv_color_hex(0x111111), 0);
  lv_obj_set_style_text_font(screen, &lv_font_montserrat_14, 0);
  lv_obj_t* title = label("Alipay E2E", 8, 6, info.width - 16);
  page.status =
      label("Connecting network / initializing...", 16, 44, info.width - 32);
  page.detail = label("Waiting for real SDK result", 16, 76, info.width - 32);
  if (title == NULL || page.status == NULL || page.detail == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  rc = h2_pal_display_set_brightness_percent(runtime->display, 80);
  if (rc != H2_PAL_OK) goto done;
  lv_refr_now(page.display);
  if (page.display_rc != H2_PAL_OK) {
    rc = page.display_rc;
    goto done;
  }
  if (h2_atomic_bool_init(&page.done, false) != H2_ATOMIC_OK) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  atomic_open = true;
  if (h2_atomic_bool_init(&page.canceled, false) != H2_ATOMIC_OK) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  cancel_open = true;
  const h2_pal_task_options_t worker = {.name = "alipay/e2e/sdk",
                                        .min_stack_size = 65536};
  rc = h2_pal_task_start(runtime->task, &worker, execute, &page, &page.worker);
  if (rc != H2_PAL_OK) goto done;
  rc = join_worker(&page);
  if (rc != H2_PAL_OK) goto done;
  present_result(&page, info.width, info.height);
  while (config->complete_binding && page.result.qr_visible) {
    if (config->binding_code_ready != NULL)
      config->binding_code_ready(runtime, config->user, &page.result);
    page.binding_result = page.result;
    h2_atomic_bool_store(&page.done, false, H2_ATOMIC_RELEASE);
    lv_label_set_text(page.status, "Scan with Alipay");
    lv_refr_now(page.display);
    rc = h2_pal_task_start(runtime->task, &worker, complete_binding, &page,
                           &page.worker);
    if (rc != H2_PAL_OK) {
      page.result.result = rc;
      break;
    } else {
      rc = join_worker(&page);
      if (rc != H2_PAL_OK) goto done;
      page.result = page.binding_result;
    }
    if (page.result.binding.state != H2_ALIPAY_BINDING_FAILED) break;
    clear_code(&page);
    lv_label_set_text(page.status, "Refreshing binding code...");
    lv_refr_now(page.display);
    page.binding_result = page.result;
    h2_atomic_bool_store(&page.done, false, H2_ATOMIC_RELEASE);
    rc = h2_pal_task_start(runtime->task, &worker, refresh_binding, &page,
                           &page.worker);
    if (rc != H2_PAL_OK) {
      page.result.result = rc;
      break;
    }
    rc = join_worker(&page);
    if (rc != H2_PAL_OK) goto done;
    page.result = page.binding_result;
    present_result(&page, info.width, info.height);
  }
  if (config->complete_binding) {
    clear_code(&page);
    lv_obj_set_hidden(page.detail, false);
    lv_label_set_text(
        page.status,
        page.result.result == H2_PAL_OK &&
                page.result.binding.state == H2_ALIPAY_BINDING_COMPLETE
            ? "Bound / credentials verified in KV"
            : "Binding incomplete / see diagnostic status");
    char text[160];
    (void)snprintf(
        text, sizeof(text),
        "Binding state: %d   Result: %d\n"
        "TID: %zu B / UID: %zu B\nKey: %zu B / seed: %zu B",
        (int)page.result.binding.state, page.result.result,
        page.result.binding.tid_length, page.result.binding.uid_length,
        page.result.binding.shared_key_length, page.result.binding.seed_length);
    lv_label_set_text(page.detail, text);
    lv_obj_set_pos(page.status, 16, 44);
    lv_refr_now(page.display);
    if (page.display_rc != H2_PAL_OK) page.result.display_rc = page.display_rc;
    if (page.result.display_rc == H2_PAL_OK && config->observe != NULL)
      page.result.display_rc = config->observe(
          config->user, page.pixels, page.stride, info.width, info.height);
  }
  uint64_t reported = 0;
  (void)h2_pal_time_get_monotonic_ms(runtime->time, &reported);
  if (config->report != NULL)
    config->report(runtime, config->user, &page.result, false);
  while (config->should_exit == NULL || !config->should_exit(config->user)) {
    rc = drain(runtime);
    if (rc != H2_PAL_OK) break;
    uint64_t current = 0;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &current);
    if (rc != H2_PAL_OK) break;
    if (current - reported >= 10000) {
      if (config->report != NULL)
        config->report(runtime, config->user, &page.result, true);
      reported = current;
    }
    int wait = h2_runtime_wait_notify(runtime, 100);
    if (wait != H2_PAL_OK && wait != H2_PAL_ERR_TIMEOUT) {
      rc = wait;
      break;
    }
  }
  if (rc == H2_PAL_OK) rc = page.result.display_rc;
done:
  if (cancel_open) h2_atomic_bool_destroy(&page.canceled);
  if (atomic_open) h2_atomic_bool_destroy(&page.done);
  if (page.display != NULL) lv_display_delete(page.display);
  if (lvgl_open) lv_deinit();
  h2_pal_mem_free(runtime->mem, page.qr_pixels);
  h2_pal_mem_free(runtime->mem, page.pixels);
  if (platform_open) h2_lvgl_platform_deinit();
  int closed = h2_pal_display_close(runtime->display);
  return rc != H2_PAL_OK ? rc : closed;
}
