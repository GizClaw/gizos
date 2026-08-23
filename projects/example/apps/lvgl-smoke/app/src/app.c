#include "h2_lvgl_smoke.h"

#include "h2_lvgl_platform.h"
#include "lvgl.h"

#include <limits.h>
#include <stdint.h>

#define H2_LVGL_SMOKE_FRAME_TIMEOUT_MS 2000u
#define H2_LVGL_SMOKE_TICK_MS 16u

typedef struct h2_lvgl_smoke_ui {
  h2_runtime_t *runtime;
  lv_display_t *display;
  lv_obj_t *progress;
  void *render_buffer;
  h2_pal_result_t flush_result;
  int width;
  int height;
  int flushed;
} h2_lvgl_smoke_ui_t;

static void display_flush(lv_display_t *display, const lv_area_t *area,
                          uint8_t *pixels) {
  h2_lvgl_smoke_ui_t *ui =
      (h2_lvgl_smoke_ui_t *)lv_display_get_user_data(display);
  if (ui == NULL || area == NULL || pixels == NULL) {
    lv_display_flush_ready(display);
    return;
  }
  const h2_display_rect_t rect = {
      .x = area->x1,
      .y = area->y1,
      .width = area->x2 - area->x1 + 1,
      .height = area->y2 - area->y1 + 1,
  };
  ui->flush_result = h2_pal_display_draw_bitmap(
      ui->runtime->display, &rect, pixels,
      (size_t)rect.width * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
  if (ui->flush_result == H2_PAL_OK) {
    ui->flush_result = h2_pal_display_present(ui->runtime->display);
  }
  ui->flushed = ui->flush_result == H2_PAL_OK && rect.x == 0 && rect.y == 0 &&
                rect.width == ui->width && rect.height == ui->height;
  lv_display_flush_ready(display);
}

static h2_pal_result_t build_screen(h2_lvgl_smoke_ui_t *ui) {
  lv_obj_t *screen = lv_screen_active();
  if (screen == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x111827u), 0);

  lv_obj_t *title = lv_label_create(screen);
  if (title == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_label_set_text(title, "LVGL SMOKE");
  lv_obj_set_style_text_color(title, lv_color_hex(0xffffffu), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

  lv_obj_t *card = lv_obj_create(screen);
  if (card == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 204, 92);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, -5);
  lv_obj_set_style_radius(card, 18, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x1f2937u), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);

  static const uint32_t colors[] = {
      0xef4444u, 0xfacc15u, 0x22c55eu, 0x22d3eeu, 0x3b82f6u, 0xd946efu,
  };
  for (size_t i = 0u; i < sizeof(colors) / sizeof(colors[0]); ++i) {
    lv_obj_t *dot = lv_obj_create(card);
    if (dot == NULL) {
      return H2_PAL_ERR_NO_MEMORY;
    }
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 22, 22);
    lv_obj_set_pos(dot, 14 + (int32_t)i * 31, 15);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(colors[i]), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  }

  ui->progress = lv_bar_create(card);
  if (ui->progress == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_obj_remove_style_all(ui->progress);
  lv_obj_set_size(ui->progress, 176, 14);
  lv_obj_set_pos(ui->progress, 14, 57);
  lv_obj_set_style_radius(ui->progress, 7, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ui->progress, lv_color_hex(0x374151u),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ui->progress, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(ui->progress, 7, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(ui->progress, lv_color_hex(0x22c55eu),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(ui->progress, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_bar_set_range(ui->progress, 0, 100);
  lv_bar_set_value(ui->progress, 0, LV_ANIM_OFF);

  lv_obj_t *detail = lv_label_create(screen);
  if (detail == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_label_set_text(detail, "FULL FRAME RGB565 / PSRAM");
  lv_obj_set_style_text_color(detail, lv_color_hex(0x9ca3afu), 0);
  lv_obj_align(detail, LV_ALIGN_BOTTOM_MID, 0, -28);
  return H2_PAL_OK;
}

static void deinit_ui(h2_lvgl_smoke_ui_t *ui, const h2_pal_mem_api_t *mem,
                      int lvgl_initialized, int platform_initialized) {
  if (ui->display != NULL) {
    lv_display_delete(ui->display);
  }
  if (lvgl_initialized) {
    lv_deinit();
  }
  if (platform_initialized) {
    h2_lvgl_platform_deinit();
  }
  h2_pal_mem_free(mem, ui->render_buffer);
  if (ui->runtime != NULL) {
    (void)h2_pal_display_close(ui->runtime->display);
  }
}

h2_pal_result_t h2_lvgl_smoke_run(h2_runtime_t *runtime,
                                  const h2_lvgl_smoke_config_t *config) {
  if (runtime == NULL || config == NULL || runtime->display == NULL ||
      runtime->task == NULL || runtime->sync == NULL ||
      runtime->queue == NULL || runtime->time == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_mem_api_t *mem =
      config->mem != NULL ? config->mem : runtime->mem;
  if (mem == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  h2_lvgl_smoke_ui_t ui = {
      .runtime = runtime,
      .flush_result = H2_PAL_OK,
  };
  h2_pal_result_t rc = h2_pal_display_open(runtime->display);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  h2_display_info_t info = {0};
  rc = h2_pal_display_get_info(runtime->display, &info);
  if (rc != H2_PAL_OK || info.width <= 0 || info.height <= 0 ||
      (size_t)info.width > SIZE_MAX / (size_t)info.height / sizeof(uint16_t)) {
    (void)h2_pal_display_close(runtime->display);
    return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : rc;
  }
  const size_t render_bytes =
      (size_t)info.width * (size_t)info.height * sizeof(uint16_t);
  if (render_bytes > UINT32_MAX) {
    (void)h2_pal_display_close(runtime->display);
    return H2_PAL_ERR_INVALID_STATE;
  }
  ui.width = info.width;
  ui.height = info.height;
  ui.render_buffer = h2_pal_mem_alloc(mem, render_bytes);
  if (ui.render_buffer == NULL) {
    (void)h2_pal_display_close(runtime->display);
    return H2_PAL_ERR_NO_MEMORY;
  }

  const h2_lvgl_platform_config_t platform = {
      .allocator = mem,
      .task_api = runtime->task,
      .sync_api = runtime->sync,
      .queue_api = runtime->queue,
      .time_api = runtime->time,
  };
  rc = h2_lvgl_platform_init(&platform);
  if (rc != H2_PAL_OK) {
    deinit_ui(&ui, mem, 0, 0);
    return rc;
  }
  lv_init();
  ui.display = lv_display_create(info.width, info.height);
  if (ui.display == NULL) {
    deinit_ui(&ui, mem, 1, 1);
    return H2_PAL_ERR_NO_MEMORY;
  }
  lv_display_set_user_data(ui.display, &ui);
  lv_display_set_color_format(ui.display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(ui.display, display_flush);
  lv_display_set_buffers(ui.display, ui.render_buffer, NULL,
                         (uint32_t)render_bytes,
                         LV_DISPLAY_RENDER_MODE_FULL);

  rc = build_screen(&ui);
  uint64_t last_ms = 0u;
  uint64_t deadline_ms = 0u;
  if (rc == H2_PAL_OK) {
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &last_ms);
    deadline_ms =
        h2_pal_time_deadline_ms(last_ms, H2_LVGL_SMOKE_FRAME_TIMEOUT_MS);
  }
  while (rc == H2_PAL_OK && !ui.flushed) {
    uint64_t now_ms = last_ms;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
    if (rc != H2_PAL_OK) {
      break;
    }
    if (h2_pal_time_deadline_expired(now_ms, deadline_ms)) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    const uint64_t elapsed = now_ms >= last_ms ? now_ms - last_ms : 0u;
    lv_tick_inc((uint32_t)(elapsed > UINT32_MAX ? UINT32_MAX : elapsed));
    last_ms = now_ms;
    (void)lv_timer_handler();
    if (ui.flush_result != H2_PAL_OK) {
      rc = ui.flush_result;
      break;
    }
    rc = h2_pal_time_sleep_ms(runtime->time, 1u);
  }
  if (rc == H2_PAL_OK && config->ready != NULL) {
    rc = config->ready(config->ready_user);
  }

  uint32_t progress = 0u;
  while (rc == H2_PAL_OK) {
    uint64_t now_ms = last_ms;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
    if (rc != H2_PAL_OK) {
      break;
    }
    uint64_t elapsed = now_ms >= last_ms ? now_ms - last_ms : 0u;
    lv_tick_inc((uint32_t)(elapsed > UINT32_MAX ? UINT32_MAX : elapsed));
    last_ms = now_ms;
    progress = (progress + 1u) % 101u;
    lv_bar_set_value(ui.progress, (int32_t)progress, LV_ANIM_OFF);
    (void)lv_timer_handler();
    if (ui.flush_result != H2_PAL_OK) {
      rc = ui.flush_result;
      break;
    }
    rc = h2_pal_time_sleep_ms(runtime->time, H2_LVGL_SMOKE_TICK_MS);
  }

  deinit_ui(&ui, mem, 1, 1);
  return rc;
}
