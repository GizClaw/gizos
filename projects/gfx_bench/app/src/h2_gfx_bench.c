#include "h2_gfx_bench.h"

#include "h2_gfx_scene.h"
#include "h2_car_model.h"
#include "h2_atomic.h"
#include "h2_runtime_event.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Tiny hand-authored 3x5 font. The same overlay is drawn after either library.
 */
static const char glyph_names[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ./-";
static const uint16_t glyphs[] = {
    0x7B6F, 0x2492, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249,
    0x7BEF, 0x7BCF, 0x2BED, 0x6BAE, 0x7927, 0x6B6E, 0x79E7, 0x79E4,
    0x796B, 0x5BED, 0x7497, 0x124E, 0x5BAD, 0x4927, 0x5FED, 0x5F6D,
    0x7B6F, 0x7BE4, 0x7B7B, 0x7BAD, 0x79CF, 0x7492, 0x5B6F, 0x5B6A,
    0x5BFD, 0x5AAD, 0x5A92, 0x72A7, 0x0002, 0x12A4, 0x01C0,
};

static uint16_t rgb(unsigned red, unsigned green, unsigned blue) {
  return (uint16_t)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
}

static void text(uint16_t* pixels, int width, int height, int x, int y,
                 const char* label, uint16_t color, int scale) {
  for (; *label; ++label, x += 4 * scale) {
    const char* found = strchr(glyph_names, *label);
    if (!found) continue;
    const uint16_t glyph = glyphs[found - glyph_names];
    for (int row = 0; row < 5; ++row)
      for (int col = 0; col < 3; ++col) {
        if (!(glyph & (1u << (14 - row * 3 - col)))) continue;
        for (int dy = 0; dy < scale; ++dy)
          for (int dx = 0; dx < scale; ++dx) {
            const int xx = x + col * scale + dx;
            const int yy = y + row * scale + dy;
            if (xx >= 0 && xx < width && yy >= 0 && yy < height)
              pixels[(size_t)yy * (size_t)width + (size_t)xx] = color;
          }
      }
  }
}

static void centered_text(uint16_t* pixels, int width, int height, int y,
                          const char* label, uint16_t color, int scale) {
  const int x = (width - (int)strlen(label) * 4 * scale) / 2;
  text(pixels, width, height, x, y, label, color, scale);
}

static void log_status(h2_runtime_t* runtime, const char* backend,
                       const char* look, unsigned fps_tenths,
                       uint64_t render_us, uint64_t display_us,
                       unsigned frames) {
  char message[H2_PAL_LOG_MESSAGE_MAX];
  snprintf(
      message, sizeof(message),
      "renderer=%s look=%s fps=%u.%u render_us=%llu display_us=%llu frames=%u",
      backend, look, fps_tenths / 10u, fps_tenths % 10u,
      (unsigned long long)(render_us / frames),
      (unsigned long long)(display_us / frames), frames);
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "gfx-bench", message);
}

typedef struct touch_sample {
  int result;
  h2_pal_touch_event_t event;
} touch_sample_t;

typedef struct touch_worker {
  h2_runtime_t* runtime;
  h2_pal_queue_t* queue;
  h2_pal_task_t* task;
  h2_atomic_bool_t stop;
} touch_worker_t;

static void sample_touch(void* user) {
  touch_worker_t* worker = user;
  h2_runtime_t* runtime = worker->runtime;
  while (!h2_atomic_bool_load(&worker->stop, H2_ATOMIC_ACQUIRE)) {
    touch_sample_t sample = {0};
    sample.result = h2_pal_touch_poll_event(runtime->touch, &sample.event);
    if (sample.result != H2_PAL_ERR_WOULD_BLOCK) {
      int rc;
      do {
        rc = h2_pal_queue_send(runtime->queue, worker->queue, &sample, 50);
      } while ((rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) &&
               !h2_atomic_bool_load(&worker->stop, H2_ATOMIC_ACQUIRE));
      if (rc || sample.result) return;
    }
    if (h2_pal_time_sleep_ms(runtime->time, 8) != H2_PAL_OK) return;
  }
}

static int touch_events(h2_runtime_t* runtime, touch_worker_t* worker,
                        h2_gfx_interaction_t* state, unsigned* backend,
                        int* changed, unsigned* count, int* touch_x,
                        int* touch_y) {
  for (unsigned i = 0; i < 64; ++i) {
    touch_sample_t sample = {0};
    const int rc = h2_pal_queue_recv(runtime->queue, worker->queue, &sample, 0);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) return 0;
    if (rc) return rc;
    if (sample.result) return sample.result;
    const h2_pal_touch_event_t event = sample.event;
    ++*count;
    *touch_x = event.x;
    *touch_y = event.y;
    h2_gfx_touch_t kind;
    switch (event.kind) {
      case H2_PAL_TOUCH_EVENT_DOWN:
        kind = H2_GFX_TOUCH_DOWN;
        break;
      case H2_PAL_TOUCH_EVENT_MOVE:
        kind = H2_GFX_TOUCH_MOVE;
        break;
      case H2_PAL_TOUCH_EVENT_UP:
        kind = H2_GFX_TOUCH_UP;
        break;
      default:
        continue;
    }
    const h2_gfx_action_t action =
        h2_gfx_interaction_touch(state, kind, (float)event.x, (float)event.y);
    if (action == H2_GFX_ACTION_NEXT_BACKEND) {
      *backend ^= 1u;
      *changed = 1;
    } else if (action == H2_GFX_ACTION_NEXT_LOOK) {
      state->view.look =
          (h2_gfx_look_t)((state->view.look + 1) % H2_GFX_LOOK_COUNT);
      *changed = 1;
    }
    if (event.kind != H2_PAL_TOUCH_EVENT_MOVE || *count % 10 == 0) {
      char message[128];
      snprintf(message, sizeof(message), "kind=%d x=%d y=%d action=%d count=%u",
               (int)event.kind, (int)event.x, (int)event.y, (int)action,
               *count);
      (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "gfx-touch",
                             message);
    }
  }
  return 0;
}

int h2_gfx_bench_run(h2_runtime_t* runtime,
                     const h2_gfx_bench_config_t* config) {
  if (!runtime || !runtime->display || !runtime->touch || !runtime->mem ||
      !runtime->time || !runtime->log || !runtime->task || !runtime->queue)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_car_model.vertices || !h2_car_model.vertex_count) {
    (void)h2_pal_log_write(
        runtime->log, H2_PAL_LOG_ERROR, "gfx-bench",
        "Car asset unavailable: prepare local data and build with --define=gfx_local_car=1");
    return H2_PAL_ERR_UNSUPPORTED;
  }
  const h2_gfx_backend_t* backends[] = {&h2_gfx_backend_jet,
                                        &h2_gfx_backend_tgx};
  const char* looks[] = {"FACTORY", "POLISHED", "CHROME"};
  int opened_display = 0, opened_touch = 0;
  unsigned initialized = 0;
  uint16_t* color = NULL;
  uint16_t* depth = NULL;
  uint16_t* sky = NULL;
  h2_display_info_t info = {0};
  touch_worker_t* worker = NULL;
  int atomic_initialized = 0;
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "gfx-init",
                         "display-open");
  int rc = h2_pal_display_open(runtime->display);
  if (rc) goto done;
  opened_display = 1;
  rc = h2_pal_display_get_info(runtime->display, &info);
  if (rc) goto done;
  if (info.width < 128 || info.width > 1024 || info.height < 128 ||
      info.height > 1024 || info.width != info.height ||
      info.native_format != H2_DISPLAY_PIXEL_RGB565) {
    rc = H2_PAL_ERR_UNSUPPORTED;
    goto done;
  }
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "gfx-init",
                         "touch-open");
  rc = h2_pal_touch_open(runtime->touch);
  if (rc) goto done;
  opened_touch = 1;
  h2_pal_touch_info_t touch_info = {0};
  rc = h2_pal_touch_get_info(runtime->touch, &touch_info);
  if (rc) goto done;
  if ((int)touch_info.width != info.width ||
      (int)touch_info.height != info.height) {
    rc = H2_PAL_ERR_INVALID_ARG;
    goto done;
  }
  const size_t bytes =
      (size_t)info.width * (size_t)info.height * sizeof(uint16_t);
  color = h2_pal_mem_alloc(runtime->mem, bytes);
  depth = h2_pal_mem_alloc(runtime->mem, bytes);
  sky = h2_pal_mem_alloc(runtime->mem, (size_t)info.height * sizeof(uint16_t));
  if (!color || !depth || !sky) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  for (int y = 0; y < info.height; ++y)
    sky[y] = rgb(14u + (unsigned)y / 12u, 18u + (unsigned)y / 10u,
                 34u + (unsigned)y / 8u);
  const h2_gfx_target_t target = {color,       depth, info.width,
                                  info.height, sky,   runtime->mem};
  for (unsigned i = 0; i < 2; ++i) {
    (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "gfx-init",
                           backends[i]->name);
    ++initialized; /* deinit also handles a partially initialized backend. */
    if (backends[i]->init(&target) != 0) {
      rc = H2_PAL_ERR_NO_MEMORY;
      goto done;
    }
  }
  worker = h2_pal_mem_alloc(runtime->mem, sizeof(*worker));
  if (!worker) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  memset(worker, 0, sizeof(*worker));
  worker->runtime = runtime;
  if (h2_atomic_bool_init(&worker->stop, false) != H2_ATOMIC_OK) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  atomic_initialized = 1;
  const h2_pal_queue_config_t queue_config = {
      .name = "gfx/touch",
      .item_size = sizeof(touch_sample_t),
      .item_count = 64,
      .allocator = runtime->mem};
  rc = h2_pal_queue_create(runtime->queue, &queue_config, &worker->queue);
  if (rc) goto done;
  const h2_pal_task_options_t task_options = {.name = "gfx/touch",
                                              .min_stack_size = 4096};
  rc = h2_pal_task_start(runtime->task, &task_options, sample_touch, worker,
                         &worker->task);
  if (rc) goto done;
  unsigned touch_count = 0;
  int touch_x = -1, touch_y = -1;
  h2_gfx_interaction_t state;
  h2_gfx_interaction_init(&state, info.width, info.height);
  const h2_display_rect_t full = {0, 0, info.width, info.height};
  unsigned backend = 0, frames = 0, fps_tenths = 0;
  uint64_t render_total = 0, display_total = 0, started = 0, previous = 0;
  int first_frame = 1;
  rc = h2_pal_time_get_monotonic_us(runtime->time, &started);
  if (rc) goto done;
  previous = started;
  for (;;) {
    h2_runtime_event_t event;
    while (h2_runtime_poll_event(runtime, &event) == H2_PAL_OK) {
    }
    int changed = 0;
    rc = touch_events(runtime, worker, &state, &backend, &changed, &touch_count,
                      &touch_x, &touch_y);
    if (rc) break;
    uint64_t begin = 0, rendered = 0, displayed = 0;
    rc = h2_pal_time_get_monotonic_us(runtime->time, &begin);
    if (rc) break;
    if (changed) {
      started = begin;
      frames = 0;
      render_total = display_total = 0;
      fps_tenths = 0;
    }
    float seconds = (float)(begin - previous) / 1000000.0f;
    if (seconds > 0.1f) seconds = 0.1f;
    h2_gfx_interaction_step(&state, seconds);
    previous = begin;
    if (first_frame)
      (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "gfx-init",
                             "first-render");
    backends[backend]->render(&state.view);
    char title[64];
    snprintf(title, sizeof(title), "%s / %s / %u.%u FPS",
             backends[backend]->name, looks[state.view.look], fps_tenths / 10u,
             fps_tenths % 10u);
    centered_text(color, info.width, info.height, 70, title, 0xFFFF, 2);
    char touch_label[48];
    snprintf(touch_label, sizeof(touch_label), "TOUCH %u  %d/%d", touch_count,
             touch_x, touch_y);
    centered_text(color, info.width, info.height, 90, touch_label, 0xFFE0, 2);
    if (touch_x >= 0 && touch_x < info.width && touch_y >= 0 &&
        touch_y < info.height) {
      for (int d = -8; d <= 8; ++d) {
        if (touch_x + d >= 0 && touch_x + d < info.width)
          color[(size_t)touch_y * (size_t)info.width + (size_t)(touch_x + d)] =
              0xFFE0;
        if (touch_y + d >= 0 && touch_y + d < info.height)
          color[(size_t)(touch_y + d) * (size_t)info.width + (size_t)touch_x] =
              0xFFE0;
      }
    }
    centered_text(color, info.width, info.height, info.height - 85,
                  "DRAG CAR TO MOVE", 0xBDF7, 2);
    centered_text(color, info.width, info.height, info.height - 65,
                  "TAP CAR / PAINT", 0x7BEF, 2);
    centered_text(color, info.width, info.height, info.height - 45,
                  "TAP BACKGROUND / ENGINE", 0x7BEF, 2);
    rc = h2_pal_time_get_monotonic_us(runtime->time, &rendered);
    if (rc) break;
    if (first_frame)
      (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "gfx-init",
                             "first-display");
    rc = h2_pal_display_draw_bitmap(runtime->display, &full, color,
                                    (size_t)info.width * 2u,
                                    H2_DISPLAY_PIXEL_RGB565);
    if (rc) break;
    rc = h2_pal_display_present(runtime->display);
    if (rc) break;
    rc = h2_pal_time_get_monotonic_us(runtime->time, &displayed);
    if (rc) break;
    render_total += rendered - begin;
    display_total += displayed - rendered;
    ++frames;
    if (first_frame) {
      first_frame = 0;
      if (config && config->ready) {
        rc = config->ready(config->user);
        if (rc) break;
      }
    }
    if (displayed - started >= 1000000u) {
      fps_tenths =
          (unsigned)((uint64_t)frames * 10000000u / (displayed - started));
      log_status(runtime, backends[backend]->name, looks[state.view.look],
                 fps_tenths, render_total, display_total, frames);
      frames = 0;
      render_total = display_total = 0;
      started = displayed;
    }
    /* Yield to recovery, touch and system tasks, and honor Runtime wakeups. */
    rc = h2_runtime_wait_notify(runtime, 1);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) rc = 0;
    if (rc) break;
  }
done:
  if (worker) {
    if (atomic_initialized)
      h2_atomic_bool_store(&worker->stop, true, H2_ATOMIC_RELEASE);
    if (worker->queue) (void)h2_pal_queue_close(runtime->queue, worker->queue);
    if (worker->task) {
      const int join_rc = h2_pal_task_join(runtime->task, worker->task);
      if (join_rc)
        return join_rc; /* Preserve context/provider borrows for a late worker.
                         */
    }
    h2_pal_queue_destroy(runtime->queue, worker->queue);
    if (atomic_initialized) h2_atomic_bool_destroy(&worker->stop);
    h2_pal_mem_free(runtime->mem, worker);
  }
  while (initialized) backends[--initialized]->deinit();
  if (opened_touch) {
    const int close_rc = h2_pal_touch_close(runtime->touch);
    if (!rc) rc = close_rc;
  }
  if (opened_display) {
    const int close_rc = h2_pal_display_close(runtime->display);
    if (!rc) rc = close_rc;
  }
  h2_pal_mem_free(runtime->mem, sky);
  h2_pal_mem_free(runtime->mem, depth);
  h2_pal_mem_free(runtime->mem, color);
  return rc;
}
