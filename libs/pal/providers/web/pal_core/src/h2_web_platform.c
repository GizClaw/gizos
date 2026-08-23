#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *h2_web_alloc(void *user, size_t size) {
  (void)user;
  return malloc(size);
}

static void *h2_web_realloc(void *user, void *memory, size_t size) {
  (void)user;
  return realloc(memory, size);
}

static void h2_web_free(void *user, void *memory) {
  (void)user;
  free(memory);
}

static const h2_pal_mem_vtable_t h2_web_mem_vtable = {
    .alloc = h2_web_alloc,
    .realloc = h2_web_realloc,
    .free = h2_web_free,
};

static const h2_pal_mem_api_t h2_web_mem_api = {
    .user = NULL,
    .vtable = &h2_web_mem_vtable,
};

EM_JS(void, h2_web_console_write,
      (int level, const char *scope, const char *message), {
        const prefix = scope ? `[${UTF8ToString(scope)}] ` : "";
        const text = prefix + UTF8ToString(message);
        if (level >= 3) console.error(text);
        else if (level === 2) console.warn(text);
        else if (level === 0) console.debug(text);
        else console.info(text);
      });

static int h2_web_log_write(void *user, h2_pal_log_level_t level,
                            const char *scope, const char *message) {
  (void)user;
  if (message == NULL || level < H2_PAL_LOG_DEBUG ||
      level > H2_PAL_LOG_ERROR) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_web_console_write((int)level, scope, message);
  return H2_PAL_OK;
}

static const h2_pal_log_vtable_t h2_web_log_vtable = {
    .write = h2_web_log_write,
};

static const h2_pal_log_api_t h2_web_log_api = {
    .user = NULL,
    .vtable = &h2_web_log_vtable,
};

static uint64_t h2_web_now_ms(void *user) {
  (void)user;
  return (uint64_t)emscripten_get_now();
}

static h2_libco_result_t h2_web_poll_external(void *user,
                                               h2_libco_t *executor) {
  return h2_web_platform_serial_poll(user, executor);
}

EM_JS(void, h2_web_pump_schedule_js,
      (uintptr_t platform_address, double delay_ms), {
        const timers = Module['h2WebPumpTimers'] ||= new Map();
        const previous = timers.get(platform_address);
        if (previous) clearTimeout(previous.timer);
        const entry = {timer: 0};
        const fire = () => {
          if (timers.get(platform_address) !== entry) return;
          if (typeof Asyncify !== 'undefined' && Asyncify.currData) {
            entry.timer = setTimeout(fire, 0);
            return;
          }
          timers.delete(platform_address);
          Promise.resolve(Module['ccall'](
              'h2_web_pump_due', null, ['number'], [platform_address],
              {async: true})).catch((error) => {
                console.error('Web PAL pump failed', error);
              });
        };
        entry.timer = setTimeout(fire, delay_ms);
        timers.set(platform_address, entry);
      });

EM_JS(void, h2_web_pump_cancel_js, (uintptr_t platform_address), {
  const timers = Module['h2WebPumpTimers'];
  const entry = timers && timers.get(platform_address);
  if (entry) {
    clearTimeout(entry.timer);
    timers.delete(platform_address);
  }
});

EMSCRIPTEN_KEEPALIVE void h2_web_pump_due(uintptr_t platform_address) {
  h2_web_platform_t *platform = (h2_web_platform_t *)platform_address;
  if (platform == NULL || !platform->pump_scheduled) {
    return;
  }
  platform->pump_scheduled = false;
  platform->pump_deadline_ms = 0u;
  if (!platform->shutting_down) {
    (void)h2_web_platform_pump(platform, 32u, NULL);
  }
}

void h2_web_platform_request_pump(h2_web_platform_t *platform,
                                  uint64_t deadline_ms) {
  if (platform == NULL || platform->shutting_down) {
    return;
  }
  if (platform->pump_scheduled) {
    if (platform->pump_deadline_ms <= deadline_ms) {
      return;
    }
    h2_web_pump_cancel_js((uintptr_t)platform);
  }
  const uint64_t now_ms = h2_web_now_ms(platform);
  const double delay_ms = deadline_ms > now_ms
                              ? (double)(deadline_ms - now_ms)
                              : 0.0;
  platform->pump_deadline_ms = deadline_ms;
  platform->pump_scheduled = true;
  h2_web_pump_schedule_js((uintptr_t)platform, delay_ms);
}

void h2_web_platform_schedule(h2_web_platform_t *platform) {
  h2_web_platform_request_pump(platform, 0u);
}

static void h2_web_idle(void *user, int has_deadline, uint64_t deadline_ms) {
  if (has_deadline) {
    h2_web_platform_request_pump(user, deadline_ms);
  }
}

h2_web_platform_t *
h2_web_platform_create(const h2_web_platform_config_t *config) {
  if (config == NULL || config->display_width <= 0 ||
      config->display_height <= 0) {
    return NULL;
  }
  const size_t width = (size_t)config->display_width;
  const size_t height = (size_t)config->display_height;
  if (width > SIZE_MAX / height ||
      width * height > SIZE_MAX / sizeof(uint32_t)) {
    return NULL;
  }
  h2_web_platform_t *platform = calloc(1u, sizeof(*platform));
  if (platform == NULL) {
    return NULL;
  }
  platform->width = config->display_width;
  platform->height = config->display_height;
  const h2_libco_config_t executor_config = {
      .user = platform,
      .alloc = h2_web_alloc,
      .free = h2_web_free,
      .now_ms = h2_web_now_ms,
      .poll_external = h2_web_poll_external,
      .idle = h2_web_idle,
  };
  if (h2_libco_create(&executor_config, &platform->executor) != H2_LIBCO_OK) {
    free(platform);
    return NULL;
  }
  h2_web_platform_timer_init(platform);
  h2_web_platform_pref_init(platform);
  h2_web_platform_display_init(platform);
  if (h2_web_platform_serial_init(platform) != H2_PAL_OK) {
    h2_web_platform_display_deinit(platform);
    h2_web_platform_timer_deinit(platform);
    (void)h2_libco_destroy(&platform->executor);
    free(platform);
    return NULL;
  }
  return platform;
}

void h2_web_platform_destroy(h2_web_platform_t *platform) {
  if (platform == NULL) {
    return;
  }
  platform->shutting_down = true;
  if (platform->pump_scheduled) {
    h2_web_pump_cancel_js((uintptr_t)platform);
    platform->pump_scheduled = false;
    platform->pump_deadline_ms = 0u;
  }
  if (h2_libco_destroy(&platform->executor) != H2_LIBCO_OK) {
    platform->shutting_down = false;
    h2_web_platform_request_pump(platform, h2_web_now_ms(platform));
    return;
  }
  h2_web_platform_serial_deinit(platform);
  h2_web_platform_display_deinit(platform);
  h2_web_platform_timer_deinit(platform);
  free(platform);
}

h2_pal_result_t h2_web_platform_pump(h2_web_platform_t *platform,
                                     size_t work_budget,
                                     size_t *out_resumed) {
  if (out_resumed != NULL) {
    *out_resumed = 0u;
  }
  if (platform == NULL || platform->executor == NULL || platform->pumping ||
      platform->shutting_down) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  platform->pumping = true;
  h2_web_platform_timer_dispatch(platform);
  size_t resumed = 0u;
  const h2_libco_result_t result =
      h2_libco_schedule(platform->executor, work_budget, &resumed);
  platform->pumping = false;
  if (out_resumed != NULL) {
    *out_resumed = resumed;
  }
  if (result == H2_LIBCO_OK) {
    if (resumed != 0u) {
      h2_web_platform_request_pump(platform, h2_web_now_ms(platform));
    }
  }
  return result == H2_LIBCO_OK ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_web_platform_task_cancel(h2_web_platform_t *platform,
                                            h2_pal_task_t *task) {
  if (platform == NULL || platform->executor == NULL || task == NULL ||
      platform->shutting_down) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_libco_pal_task_cancel(platform->executor, task);
}

const h2_pal_mem_api_t *h2_web_platform_mem_api(void) {
  return &h2_web_mem_api;
}

const h2_pal_log_api_t *h2_web_platform_log_api(void) {
  return &h2_web_log_api;
}

const h2_pal_time_api_t *
h2_web_platform_time_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : h2_libco_time_api(platform->executor);
}

const h2_pal_task_api_t *
h2_web_platform_task_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : h2_libco_task_api(platform->executor);
}

const h2_pal_queue_api_t *
h2_web_platform_queue_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : h2_libco_queue_api(platform->executor);
}

const h2_pal_sync_api_t *
h2_web_platform_sync_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : h2_libco_sync_api(platform->executor);
}

const h2_pal_pref_api_t *
h2_web_platform_pref_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->pref_api;
}

const h2_pal_timer_api_t *
h2_web_platform_timer_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->timer_api;
}

const h2_pal_display_api_t *
h2_web_platform_display_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->display_api;
}

const h2_pal_touch_api_t *
h2_web_platform_touch_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->touch_api;
}

const h2_pal_serial_host_api_t *
h2_web_platform_serial_host_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->serial_api;
}
