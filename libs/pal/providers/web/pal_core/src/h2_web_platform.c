#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "h2_web_main_thread.h"
#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic size_t h2_web_allocations;
static _Atomic size_t h2_web_allocation_bytes;

static void *h2_web_alloc(void *user, size_t size) {
  (void)user;
  void *memory = malloc(size);
  if (memory != NULL) {
    ++h2_web_allocations;
    h2_web_allocation_bytes += malloc_usable_size(memory);
  }
  return memory;
}

static void *h2_web_realloc(void *user, void *memory, size_t size) {
  if (memory == NULL)
    return h2_web_alloc(user, size);
  const size_t old_size = malloc_usable_size(memory);
  if (size == 0u) {
    --h2_web_allocations;
    h2_web_allocation_bytes -= old_size;
    free(memory);
    return NULL;
  }
  void *next = realloc(memory, size);
  if (next != NULL)
    h2_web_allocation_bytes += malloc_usable_size(next) - old_size;
  return next;
}

static void h2_web_free(void *user, void *memory) {
  (void)user;
  if (memory == NULL)
    return;
  --h2_web_allocations;
  h2_web_allocation_bytes -= malloc_usable_size(memory);
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

/* clang-format off */
EM_JS(void, h2_web_console_write,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32", "pointer", "pointer"], null,
    (level, scope, message) => {
        const prefix = scope ? `[${UTF8ToString(scope)}] ` : "";
        const text = prefix + UTF8ToString(message);
        if (level >= 3) console.error(text);
        else if (level === 2) console.warn(text);
        else if (level === 0) console.debug(text);
        else console.info(text);
      });
});
/* clang-format on */

static int h2_web_log_write(void *user, h2_pal_log_level_t level,
                            const char *scope, const char *message) {
  (void)user;
  if (message == NULL || level < H2_PAL_LOG_DEBUG ||
      level > H2_PAL_LOG_ERROR) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  (void)h2_web_main_call(h2_web_console_write,
                         (const void *[]){&(int){(int)level},
                                          &(const char *){scope},
                                          &(const char *){message}});
  return H2_PAL_OK;
}

static const h2_pal_log_vtable_t h2_web_log_vtable = {
    .write = h2_web_log_write,
};

static const h2_pal_log_api_t h2_web_log_api = {
    .user = NULL,
    .vtable = &h2_web_log_vtable,
};

/* clang-format off */
EM_JS(void, h2_web_wall_now_ms,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "double",
    () => { return Date.now(); });
});
/* clang-format on */

static h2_pal_result_t h2_web_get_monotonic_us(void *user, uint64_t *out_us) {
  (void)user;
  if (out_us == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_us = (uint64_t)(emscripten_get_now() * 1000.0);
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_get_monotonic_ms(void *user, uint64_t *out_ms) {
  (void)user;
  if (out_ms == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_ms = (uint64_t)emscripten_get_now();
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_get_wall_ms(void *user, uint64_t *out_ms) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || out_ms == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_ms = 0u;
  const double value =
      ((double)h2_web_main_call(h2_web_wall_now_ms, NULL).f64) +
      (double)platform->wall_offset_ms;
  if (!(value >= 0.0 && value <= 9007199254740991.0))
    return H2_PAL_ERR_UNAVAILABLE;
  *out_ms = (uint64_t)value;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_set_wall_ms(void *user, uint64_t wall_ms) {
  h2_web_platform_t *platform = user;
  if (platform == NULL || wall_ms > UINT64_C(9007199254740991))
    return H2_PAL_ERR_INVALID_ARG;
  const double host = ((double)h2_web_main_call(h2_web_wall_now_ms, NULL).f64);
  if (!(host >= 0.0 && host <= 9007199254740991.0))
    return H2_PAL_ERR_UNAVAILABLE;
  platform->wall_offset_ms = (int64_t)wall_ms - (int64_t)host;
  platform->wall_user_calibrated = true;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_get_wall_status(void *user, h2_pal_time_wall_status_t *out_status) {
  if (out_status == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uint64_t wall_ms = 0u;
  h2_pal_result_t rc = h2_web_get_wall_ms(user, &wall_ms);
  *out_status = (h2_pal_time_wall_status_t){
      .valid = rc == H2_PAL_OK,
      .source = ((h2_web_platform_t *)user)->wall_user_calibrated
                    ? H2_PAL_TIME_WALL_SOURCE_USER
                    : H2_PAL_TIME_WALL_SOURCE_UNKNOWN,
  };
  return rc;
}

static h2_pal_result_t h2_web_sleep(void *user, uint32_t ms) {
  return h2_web_platform_sleep_ms(user, ms);
}

static const h2_pal_time_vtable_t h2_web_clock_vtable = {
    .sleep_ms = h2_web_sleep,
    .get_monotonic_ms = h2_web_get_monotonic_ms,
    .get_monotonic_us = h2_web_get_monotonic_us,
    .get_wall_ms = h2_web_get_wall_ms,
    .set_wall_ms = h2_web_set_wall_ms,
    .get_wall_status = h2_web_get_wall_status,
};

static h2_pal_result_t h2_web_firmware_current(void *user,
                                               h2_pal_firmware_info_t *out) {
  h2_web_platform_t *platform = user;
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (platform == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (platform->firmware_info.version[0] == '\0')
    return H2_PAL_ERR_UNSUPPORTED;
  *out = platform->firmware_info;
  return H2_PAL_OK;
}
static const h2_pal_firmware_info_vtable_t h2_web_firmware_vtable = {
    .get_current = h2_web_firmware_current,
};

static void *h2_web_pump_worker(void *argument) {
  h2_web_platform_t *platform = argument;
  pthread_mutex_lock(&platform->pump_mutex);
  while (!platform->pump_stop) {
    while (!platform->pump_scheduled && !platform->pump_stop)
      pthread_cond_wait(&platform->pump_changed, &platform->pump_mutex);
    if (platform->pump_stop)
      break;
    platform->pump_scheduled = false;
    pthread_mutex_unlock(&platform->pump_mutex);
    (void)h2_web_platform_pump(platform, 32u, NULL);
    pthread_mutex_lock(&platform->pump_mutex);
  }
  pthread_mutex_unlock(&platform->pump_mutex);
  return NULL;
}

void h2_web_platform_request_pump(h2_web_platform_t *platform,
                                  uint64_t deadline_ms) {
  (void)deadline_ms;
  if (platform == NULL)
    return;
  pthread_mutex_lock(&platform->pump_mutex);
  if (!platform->pump_stop) {
    platform->pump_scheduled = true;
    pthread_cond_signal(&platform->pump_changed);
  }
  pthread_mutex_unlock(&platform->pump_mutex);
}

void h2_web_platform_schedule(h2_web_platform_t *platform) {
  h2_web_platform_request_pump(platform, 0u);
}

h2_web_platform_t *
h2_web_platform_create(const h2_web_platform_config_t *config) {
  return h2_web_platform_create_with_task_allocator(config, NULL);
}

h2_web_platform_t *h2_web_platform_create_with_task_allocator(
    const h2_web_platform_config_t *config, const h2_pal_mem_api_t *allocator) {
  if (allocator != NULL &&
      (allocator->vtable == NULL || allocator->vtable->alloc == NULL ||
       allocator->vtable->free == NULL))
    return NULL;
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
  platform->task_allocator = allocator != NULL ? *allocator : h2_web_mem_api;
  platform->clock_api =
      (h2_pal_time_api_t){
      .user = platform,
      .vtable = &h2_web_clock_vtable};
  platform->firmware_info_api = (h2_pal_firmware_info_api_t){
      .user = platform, .vtable = &h2_web_firmware_vtable};
  pthread_mutex_init(&platform->async_mutex, NULL);
  pthread_cond_init(&platform->async_changed, NULL);
  pthread_mutex_init(&platform->event_mutex, NULL);
  pthread_cond_init(&platform->event_changed, NULL);
  pthread_mutex_init(&platform->pump_mutex, NULL);
  pthread_cond_init(&platform->pump_changed, NULL);
  if (h2_web_thread_core_create(&platform->task_allocator,
                                &platform->executor) != H2_PAL_OK) {
    free(platform);
    return NULL;
  }
  h2_web_platform_pref_init(platform);
  h2_web_platform_http_init(platform);
  if (h2_web_platform_crypto_init(platform) != H2_PAL_OK) {
    (void)h2_web_thread_core_destroy(&platform->executor);
    free(platform);
    return NULL;
  }
  h2_web_platform_audio_init(platform);
  h2_web_platform_audio_decoder_init(platform);
  h2_web_platform_video_decoder_init(platform);
  h2_web_platform_display_init(platform);
  h2_web_platform_netif_init(platform);
  h2_web_platform_webrtc_init(platform);
  if (h2_web_platform_serial_init(platform) != H2_PAL_OK) {
    h2_web_platform_webrtc_deinit(platform);
    h2_web_platform_netif_deinit(platform);
    h2_web_platform_display_deinit(platform);
    h2_web_platform_audio_deinit(platform);
    h2_web_platform_crypto_deinit(platform);
    (void)h2_web_thread_core_destroy(&platform->executor);
    free(platform);
    return NULL;
  }
  if (pthread_create(&platform->pump_thread, NULL, h2_web_pump_worker,
                     platform) != 0) {
    h2_web_platform_destroy(platform);
    return NULL;
  }
  return platform;
}

h2_pal_result_t h2_web_platform_destroy(h2_web_platform_t *platform) {
  if (platform == NULL) {
    return H2_PAL_OK;
  }
  if (platform->executor != NULL) {
    // The owner serializes destruction against public callers. Internal event
    // work is admitted under pump_mutex so it cannot race executor teardown.
    h2_web_platform_webrtc_reap(platform);
    pthread_mutex_lock(&platform->event_mutex);
    bool busy = platform->system_event_posts != 0u ||
                platform->system_event_unsubscribe_waiters != 0u;
    pthread_mutex_unlock(&platform->event_mutex);
    pthread_mutex_lock(&platform->async_mutex);
    busy |= platform->async_waiters != 0u || platform->async_ops != NULL;
    pthread_mutex_unlock(&platform->async_mutex);
    h2_web_state_enter();
    busy |= platform->mic_calls != 0u || platform->http_requests != 0u ||
            platform->audio_tracks != NULL ||
            platform->open_filesystems != 0u ||
            h2_web_platform_webrtc_busy(platform);
    int unused;
    h2_web_state_leave(&unused);
    if (busy)
      return H2_PAL_ERR_BUSY;
    pthread_mutex_lock(&platform->pump_mutex);
    if (platform->pumping) {
      pthread_mutex_unlock(&platform->pump_mutex);
      return H2_PAL_ERR_BUSY;
    }
    platform->shutting_down = true;
    if (h2_web_thread_core_destroy(&platform->executor) != H2_PAL_OK) {
    platform->shutting_down = false;
      pthread_mutex_unlock(&platform->pump_mutex);
      return H2_PAL_ERR_BUSY;
    }
    platform->pump_stop = true;
    pthread_cond_broadcast(&platform->pump_changed);
    pthread_mutex_unlock(&platform->pump_mutex);
  }
  if (platform->pump_thread) {
    int joined = emscripten_is_main_runtime_thread()
                     ? pthread_tryjoin_np(platform->pump_thread, NULL)
                     : pthread_join(platform->pump_thread, NULL);
    if (joined != 0)
      return H2_PAL_ERR_BUSY;
    platform->pump_thread = 0;
  }
  h2_web_platform_serial_deinit(platform);
  h2_web_platform_webrtc_deinit(platform);
  h2_web_platform_netif_deinit(platform);
  h2_web_platform_display_deinit(platform);
  h2_web_platform_audio_deinit(platform);
  h2_web_platform_crypto_deinit(platform);
  pthread_cond_destroy(&platform->async_changed);
  pthread_mutex_destroy(&platform->async_mutex);
  pthread_cond_destroy(&platform->event_changed);
  pthread_mutex_destroy(&platform->event_mutex);
  pthread_cond_destroy(&platform->pump_changed);
  pthread_mutex_destroy(&platform->pump_mutex);
  free(platform);
  return H2_PAL_OK;
}

h2_pal_result_t h2_web_platform_pump(h2_web_platform_t *platform,
                                     size_t work_budget,
                                     size_t *out_resumed) {
  if (out_resumed != NULL) {
    *out_resumed = 0u;
  }
  (void)work_budget;
  if (platform == NULL || platform->shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  if (emscripten_is_main_runtime_thread()) {
    h2_web_platform_request_pump(platform, 0u);
    return H2_PAL_OK;
  }
  pthread_mutex_lock(&platform->pump_mutex);
  while (platform->pumping) {
    if (pthread_equal(platform->pump_owner, pthread_self())) {
      pthread_mutex_unlock(&platform->pump_mutex);
      return H2_PAL_ERR_INVALID_STATE;
    }
    pthread_cond_wait(&platform->pump_changed, &platform->pump_mutex);
  }
  if (platform->shutting_down) {
    pthread_mutex_unlock(&platform->pump_mutex);
    return H2_PAL_ERR_INVALID_STATE;
  }
  platform->pumping = true;
  platform->pump_owner = pthread_self();
  pthread_mutex_unlock(&platform->pump_mutex);
  h2_web_platform_webrtc_reap(platform);
  h2_web_platform_event_retire(platform);
  h2_web_platform_netif_poll(platform);
  pthread_mutex_lock(&platform->pump_mutex);
  platform->pumping = false;
  pthread_cond_broadcast(&platform->pump_changed);
  pthread_mutex_unlock(&platform->pump_mutex);
  return H2_PAL_OK;
}

h2_pal_result_t h2_web_platform_task_cancel(h2_web_platform_t *platform,
                                            h2_pal_task_t *task) {
  if (platform == NULL || platform->executor == NULL || task == NULL ||
      platform->shutting_down) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_web_thread_core_task_cancel(platform->executor, task);
}

const h2_pal_mem_api_t *h2_web_platform_mem_api(void) {
  return &h2_web_mem_api;
}

h2_pal_result_t
h2_web_platform_get_resource_stats(h2_web_platform_t *platform,
                                   h2_web_platform_resource_stats_t *out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (platform == NULL || platform->shutting_down)
    return H2_PAL_ERR_INVALID_ARG;
  h2_web_thread_core_resource_stats_t stats;
  if (h2_web_thread_core_get_resource_stats(platform->executor, &stats) !=
      H2_PAL_OK)
    return H2_PAL_ERR_INVALID_STATE;
  out->live_tasks = stats.live_tasks;
  out->task_stack_bytes = stats.task_stack_bytes;
  out->live_queues = stats.live_queues;
  out->live_mutexes = stats.live_mutexes;
  out->live_semaphores = stats.live_semaphores;
  out->live_conditions = stats.live_conditions;
  out->live_timers = stats.live_timers;
  out->live_firmware_infos = platform->firmware_info.version[0] != '\0';
  pthread_mutex_lock(&platform->event_mutex);
  out->live_event_subscriptions = platform->system_event_subscription_count;
  pthread_mutex_unlock(&platform->event_mutex);
  out->allocations = h2_web_allocations;
  out->allocation_bytes = h2_web_allocation_bytes;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_web_platform_configure_firmware_info(h2_web_platform_t *platform,
                                        const char *version) {
  if (platform == NULL || version == NULL || version[0] == '\0')
    return H2_PAL_ERR_INVALID_ARG;
  if (platform->shutting_down || platform->firmware_info.version[0] != '\0')
    return H2_PAL_ERR_INVALID_STATE;
  size_t length = 0u;
  while (length < H2_PAL_FIRMWARE_VERSION_MAX && version[length] != '\0')
    ++length;
  if (length == H2_PAL_FIRMWARE_VERSION_MAX)
    return H2_PAL_ERR_TRUNCATED;
  memcpy(platform->firmware_info.version, version, length + 1u);
  return H2_PAL_OK;
}

const h2_pal_firmware_info_api_t *
h2_web_platform_firmware_info_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->firmware_info_api;
}

const h2_pal_log_api_t *h2_web_platform_log_api(void) {
  return &h2_web_log_api;
}

const h2_pal_time_api_t *
h2_web_platform_time_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->clock_api;
}

const h2_pal_task_api_t *
h2_web_platform_task_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : h2_web_thread_core_task_api(platform->executor);
}

const h2_pal_queue_api_t *
h2_web_platform_queue_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : h2_web_thread_core_queue_api(platform->executor);
}

const h2_pal_sync_api_t *
h2_web_platform_sync_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : h2_web_thread_core_sync_api(platform->executor);
}

const h2_pal_pref_api_t *
h2_web_platform_pref_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->pref_api;
}

const h2_pal_timer_api_t *
h2_web_platform_timer_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : h2_web_thread_core_timer_api(platform->executor);
}

const h2_pal_display_api_t *
h2_web_platform_display_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->display_api;
}

const h2_pal_audio_api_t *
h2_web_platform_audio_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->audio_api;
}

const h2_pal_video_decoder_api_t *
h2_web_platform_video_decoder_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->video_decoder_api;
}

const h2_pal_audio_decoder_api_t *
h2_web_platform_audio_decoder_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->audio_decoder_api;
}

const h2_pal_touch_api_t *
h2_web_platform_touch_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->touch_api;
}

const h2_pal_serial_host_api_t *
h2_web_platform_serial_host_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->serial_api;
}

const h2_pal_webrtc_api_t *
h2_web_platform_webrtc_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->webrtc_api;
}

const h2_pal_netif_api_t *
h2_web_platform_netif_api(h2_web_platform_t *platform) {
  return platform == NULL ? NULL : &platform->netif_api;
}

const h2_pal_system_event_api_t *
h2_web_platform_system_event_api(h2_web_platform_t *platform) {
  return platform == NULL || emscripten_is_main_runtime_thread()
             ? NULL : &platform->system_event_api;
}
