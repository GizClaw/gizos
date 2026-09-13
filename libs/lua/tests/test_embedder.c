#define _POSIX_C_SOURCE 200809L
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_lua_capability.h"
#include "h2_lua_event.h"
#include "h2_lua_job.h"
#include "h2_runtime_input_button.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* This test owns its OS services: no GizOS platform provider is linked. */
static void *mem_alloc(void *user, size_t size) {
  (void)user;
  return malloc(size);
}
static void *mem_realloc(void *user, void *ptr, size_t size) {
  (void)user;
  return realloc(ptr, size);
}
static void mem_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}
static const h2_pal_mem_vtable_t mem_vtable = {mem_alloc, mem_realloc,
                                               mem_free};
static const h2_pal_mem_api_t mem_api = {.vtable = &mem_vtable};

static h2_pal_result_t monotonic_us(void *user, uint64_t *out) {
  struct timespec now;
  (void)user;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  *out = (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
  return H2_PAL_OK;
}
static h2_pal_result_t monotonic_ms(void *user, uint64_t *out) {
  monotonic_us(user, out);
  *out /= 1000u;
  return H2_PAL_OK;
}
static h2_pal_result_t sleep_ms(void *user, uint32_t ms) {
  (void)user;
  struct timespec delay = {ms / 1000u, (long)(ms % 1000u) * 1000000L};
  while (nanosleep(&delay, &delay) != 0)
    assert(errno == EINTR);
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vtable = {
    .get_monotonic_ms = monotonic_ms,
    .get_monotonic_us = monotonic_us,
    .sleep_ms = sleep_ms};
static const h2_pal_time_api_t time_api = {.vtable = &time_vtable};

struct h2_pal_task {
  pthread_t thread;
  h2_pal_task_entry_t entry;
  void *ctx;
};
static void *task_entry(void *arg) {
  h2_pal_task_t *task = arg;
  task->entry(task->ctx);
  return NULL;
}
static int task_start(void *user, const h2_pal_task_options_t *options,
                      h2_pal_task_entry_t entry, void *ctx,
                      h2_pal_task_t **out) {
  (void)user;
  (void)options;
  *out = calloc(1, sizeof(**out));
  assert(*out != NULL);
  (*out)->entry = entry;
  (*out)->ctx = ctx;
  assert(pthread_create(&(*out)->thread, NULL, task_entry, *out) == 0);
  return H2_PAL_OK;
}
static int task_join(void *user, h2_pal_task_t *task) {
  (void)user;
  assert(pthread_join(task->thread, NULL) == 0);
  free(task);
  return H2_PAL_OK;
}
static const h2_pal_task_vtable_t task_vtable = {task_start, task_join};
static const h2_pal_task_api_t task_api = {.vtable = &task_vtable};

struct h2_pal_mutex {
  pthread_mutex_t mutex;
};
static h2_pal_result_t mutex_create(void *user,
                                    const h2_pal_mutex_config_t *cfg,
                                    h2_pal_mutex_t **out) {
  (void)user;
  pthread_mutexattr_t attr;
  assert(pthread_mutexattr_init(&attr) == 0);
  if (cfg->flags & H2_PAL_MUTEX_FLAG_RECURSIVE)
    assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
  *out = malloc(sizeof(**out));
  assert(*out != NULL);
  assert(pthread_mutex_init(&(*out)->mutex, &attr) == 0);
  assert(pthread_mutexattr_destroy(&attr) == 0);
  return H2_PAL_OK;
}
static h2_pal_result_t mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  assert(pthread_mutex_destroy(&mutex->mutex) == 0);
  free(mutex);
  return H2_PAL_OK;
}
static h2_pal_result_t mutex_lock(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  assert(pthread_mutex_lock(&mutex->mutex) == 0);
  return H2_PAL_OK;
}
static h2_pal_result_t mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  assert(pthread_mutex_unlock(&mutex->mutex) == 0);
  return H2_PAL_OK;
}
static const h2_pal_sync_vtable_t sync_vtable = {.create_mutex = mutex_create,
                                                 .destroy_mutex = mutex_destroy,
                                                 .lock_mutex = mutex_lock,
                                                 .unlock_mutex = mutex_unlock};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable};

struct h2_pal_queue {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  size_t size, capacity, count, head;
  int closed;
  unsigned char data[];
};
static int queue_create(void *user, const h2_pal_queue_config_t *cfg,
                        h2_pal_queue_t **out) {
  (void)user;
  *out = calloc(1, sizeof(**out) + cfg->item_size * cfg->item_count);
  assert(*out != NULL);
  (*out)->size = cfg->item_size;
  (*out)->capacity = cfg->item_count;
  assert(pthread_mutex_init(&(*out)->mutex, NULL) == 0);
  assert(pthread_cond_init(&(*out)->changed, NULL) == 0);
  return H2_PAL_OK;
}
static void queue_destroy(void *user, h2_pal_queue_t *q) {
  (void)user;
  assert(pthread_cond_destroy(&q->changed) == 0);
  assert(pthread_mutex_destroy(&q->mutex) == 0);
  free(q);
}
/* One absolute deadline preserves timeout across spurious condition wakeups. */
static struct timespec deadline(uint32_t ms) {
  struct timespec end;
  assert(clock_gettime(CLOCK_REALTIME, &end) == 0);
  end.tv_sec += ms / 1000u;
  end.tv_nsec += (long)(ms % 1000u) * 1000000L;
  end.tv_sec += end.tv_nsec / 1000000000L;
  end.tv_nsec %= 1000000000L;
  return end;
}
static int queue_transfer(h2_pal_queue_t *q, void *item, uint32_t ms, int send,
                          int latest) {
  struct timespec end = deadline(ms);
  int result = H2_PAL_OK;
  assert(pthread_mutex_lock(&q->mutex) == 0);
  if (latest && q->count == q->capacity) {
    q->head = (q->head + 1) % q->capacity;
    --q->count;
  }
  while (!q->closed && (send ? q->count == q->capacity : q->count == 0)) {
    if (ms == 0) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    int rc = ms == UINT32_MAX
                 ? pthread_cond_wait(&q->changed, &q->mutex)
                 : pthread_cond_timedwait(&q->changed, &q->mutex, &end);
    if (rc == ETIMEDOUT) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    assert(rc == 0);
  }
  if (q->closed)
    result = H2_PAL_ERR_CLOSED;
  if (result == H2_PAL_OK) {
    if (send) {
      memcpy(q->data + ((q->head + q->count) % q->capacity) * q->size, item,
             q->size);
      ++q->count;
    } else {
      memcpy(item, q->data + q->head * q->size, q->size);
      q->head = (q->head + 1) % q->capacity;
      --q->count;
    }
    assert(pthread_cond_broadcast(&q->changed) == 0);
  }
  assert(pthread_mutex_unlock(&q->mutex) == 0);
  return result;
}
static int queue_send(void *user, h2_pal_queue_t *q, const void *item,
                      uint32_t ms) {
  (void)user;
  return queue_transfer(q, (void *)item, ms, 1, 0);
}
static int queue_latest(void *user, h2_pal_queue_t *q, const void *item) {
  (void)user;
  return queue_transfer(q, (void *)item, 0, 1, 1);
}
static int queue_recv(void *user, h2_pal_queue_t *q, void *item, uint32_t ms) {
  (void)user;
  return queue_transfer(q, item, ms, 0, 0);
}
static int queue_close(void *user, h2_pal_queue_t *q) {
  (void)user;
  assert(pthread_mutex_lock(&q->mutex) == 0);
  q->closed = 1;
  assert(pthread_cond_broadcast(&q->changed) == 0);
  assert(pthread_mutex_unlock(&q->mutex) == 0);
  return H2_PAL_OK;
}
static const h2_pal_queue_vtable_t queue_vtable = {.create = queue_create,
                                                   .destroy = queue_destroy,
                                                   .send = queue_send,
                                                   .send_latest = queue_latest,
                                                   .recv = queue_recv,
                                                   .close = queue_close};
static const h2_pal_queue_api_t queue_api = {.vtable = &queue_vtable};

static struct {
  uint16_t pixels[240u * 240u];
  h2_display_rect_t rect;
  unsigned draws, presents;
} display;
static int display_open(void *user) {
  assert(user == &display);
  return H2_PAL_OK;
}
static int display_info(void *user, h2_display_info_t *info) {
  assert(user == &display);
  *info = (h2_display_info_t){240, 240, H2_DISPLAY_PIXEL_RGB565};
  return H2_PAL_OK;
}
static int display_draw(void *user, const h2_display_rect_t *rect,
                        const void *pixels, size_t stride,
                        h2_display_pixel_format_t format) {
  assert(user == &display);
  assert(format == H2_DISPLAY_PIXEL_RGB565);
  assert(rect->x >= 0 && rect->y >= 0);
  assert(rect->x + rect->width <= 240 && rect->y + rect->height <= 240);
  display.rect = *rect;
  ++display.draws;
  for (int y = 0; y < rect->height; ++y)
    memcpy(&display.pixels[(rect->y + y) * 240 + rect->x],
           (const unsigned char *)pixels + (size_t)y * stride,
           (size_t)rect->width * 2u);
  return H2_PAL_OK;
}
static int display_present(void *user) {
  assert(user == &display);
  ++display.presents;
  return H2_PAL_OK;
}
static const h2_pal_display_vtable_t display_vtable = {
    .open = display_open,
    .get_info = display_info,
    .draw_bitmap = display_draw,
    .present = display_present,
    .close = display_open};
static const h2_pal_display_api_t display_api = {&display, &display_vtable};
static h2_pal_result_t touch_open(void *user) {
  (void)user;
  return H2_PAL_OK;
}
static h2_pal_result_t touch_info(void *user, h2_pal_touch_info_t *out) {
  (void)user;
  *out = (h2_pal_touch_info_t){240, 240};
  return H2_PAL_OK;
}
static h2_pal_result_t touch_poll(void *user, h2_pal_touch_event_t *out) {
  (void)user;
  (void)out;
  return H2_PAL_ERR_WOULD_BLOCK;
}
static const h2_pal_touch_vtable_t touch_vtable = {touch_open, touch_info,
                                                   touch_poll, touch_open};
static const h2_pal_touch_api_t touch_api = {.vtable = &touch_vtable};
static const h2_pal_periph_single_button_payload_t button_payload = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE};
static const h2_pal_periph_info_t buttons[] = {
    {.id = 1,
     .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
     .name = "ok",
     .payload = &button_payload,
     .payload_size = sizeof(button_payload)},
    {.id = 2,
     .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
     .name = "back",
     .payload = &button_payload,
     .payload_size = sizeof(button_payload)}};
static h2_pal_result_t periph_list(void *user, h2_pal_periph_type_t filter,
                                   h2_pal_periph_cb_t cb, void *ctx) {
  (void)user;
  for (size_t i = 0; i < 2; ++i) {
    if (filter != H2_PAL_PERIPH_TYPE_ANY && filter != buttons[i].type)
      continue;
    h2_pal_result_t rc = cb(ctx, &buttons[i]);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t periph_get(void *user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t *out) {
  (void)user;
  if (id < 1 || id > 2)
    return H2_PAL_ERR_NOT_FOUND;
  *out = buttons[id - 1];
  return H2_PAL_OK;
}
static const h2_pal_periph_vtable_t periph_vtable = {periph_list, periph_get};
static const h2_pal_periph_api_t periph_api = {.vtable = &periph_vtable};
static h2_pal_result_t button_read(void *user, h2_pal_periph_id_t id,
                                   h2_pal_single_button_reading_t *out) {
  (void)user;
  if (id < 1 || id > 2)
    return H2_PAL_ERR_NOT_FOUND;
  *out = (h2_pal_single_button_reading_t){id, H2_PAL_BUTTON_STATE_RELEASED};
  return H2_PAL_OK;
}
static const h2_pal_button_vtable_t button_vtable = {.read_single_button =
                                                         button_read};
static const h2_pal_button_api_t button_api = {.vtable = &button_vtable};
static h2_pal_result_t map_list(void *user, h2_runtime_component_t filter,
                                h2_runtime_component_mapping_cb_t cb,
                                void *ctx) {
  (void)user;
  if (filter != H2_RUNTIME_COMPONENT_BUTTON)
    return H2_PAL_OK;
  for (uint32_t id = 1; id <= 2; ++id) {
    const h2_runtime_component_mapping_entry_t entry = {id, id};
    h2_pal_result_t rc = cb(ctx, &entry);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t map_get(void *user, h2_runtime_component_id_t id,
                               h2_pal_periph_id_t *out) {
  (void)user;
  if (id < 1 || id > 2)
    return H2_PAL_ERR_NOT_FOUND;
  *out = id;
  return H2_PAL_OK;
}
static const h2_runtime_component_mapper_vtable_t mapper_vtable = {map_list,
                                                                   map_get};
static const h2_runtime_component_mapper_t mapper = {.vtable = &mapper_vtable};

static h2_runtime_t *create_runtime(void) {
  h2_runtime_config_t cfg = {
      .board = "embedder-240",
      .target = "host",
      .chip = "fake",
      .mem = &mem_api,
      .time = &time_api,
      .task = &task_api,
      .sync = &sync_api,
      .queue = &queue_api,
      .display = &display_api,
      .touch = &touch_api,
      .button = &button_api,
      .periph = &periph_api,
      .component_mapper = &mapper,
      .firmware_info = h2_pal_unsupported_firmware_info_api(),
      .timer = h2_pal_unsupported_timer_api(),
      .disk = h2_pal_unsupported_disk_api(),
      .pref = h2_pal_unsupported_pref_api(),
      .crypto = h2_pal_unsupported_crypto_api(),
      .http = h2_pal_unsupported_http_api(),
      .net = h2_pal_unsupported_net_api(),
      .netif = h2_pal_unsupported_netif_api(),
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_pal_unsupported_webrtc_api(),
      .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
      .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
      .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
      .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
      .ble_host = h2_pal_unsupported_ble_host_api(),
      .modem = h2_pal_unsupported_modem_api(),
      .power = h2_pal_unsupported_power_api(),
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc = h2_pal_unsupported_nfc_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_pal_unsupported_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
      .input = h2_pal_unsupported_input_api(),
      .system_event = h2_pal_unsupported_system_event_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
      .fs = h2_pal_unsupported_fs_api(),
      .audio = h2_pal_unsupported_audio_api(),
      .log = h2_pal_unsupported_log_api(),
  };
  h2_runtime_t *runtime = NULL;
  assert(h2_runtime_init(&cfg, &runtime) == H2_PAL_OK);
  assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
  return runtime;
}

typedef struct pending_call {
  atomic_uint_fast64_t id;
  atomic_uint cancels;
  char input[128];
} pending_call_t;
static h2_pal_result_t capability_call(void *user,
                                       h2_lua_capability_request_id_t id,
                                       const char *input, const char *options,
                                       char *output, size_t capacity,
                                       const char **error) {
  pending_call_t *pending = user;
  (void)options;
  (void)output;
  (void)capacity;
  (void)error;
  assert(strlen(input) < sizeof(pending->input));
  strcpy(pending->input, input);
  atomic_store(&pending->id, id); /* Release publishes the copied payload. */
  return H2_PAL_ERR_WOULD_BLOCK;
}
static void capability_cancel(void *user, h2_lua_capability_request_id_t id) {
  pending_call_t *pending = user;
  assert(atomic_load(&pending->id) == id);
  atomic_fetch_add(&pending->cancels, 1);
}
static h2_lua_job_id_t submit(h2_lua_host_t *host, const char *script) {
  h2_lua_job_id_t id;
  assert(h2_lua_job_submit_text(host, NULL, "@embedder.lua",
                                (const uint8_t *)script, strlen(script), NULL,
                                0, &id) == H2_PAL_OK);
  return id;
}
static h2_lua_job_status_t status(h2_lua_host_t *host, h2_lua_job_id_t id) {
  h2_lua_job_status_t out;
  assert(h2_lua_job_get_status(host, id, &out) == H2_PAL_OK);
  return out;
}
static void wait_state(h2_lua_host_t *host, h2_lua_job_id_t id,
                       h2_lua_job_state_t wanted) {
  for (unsigned i = 0; i < 5000; ++i) {
    h2_lua_job_status_t out = status(host, id);
    if (out.state == wanted)
      return;
    if (out.state >= H2_LUA_JOB_SUCCEEDED) {
      fprintf(stderr, "unexpected job state %d: %s\n", out.state, out.message);
      abort();
    }
    sleep_ms(NULL, 1);
  }
  assert(!"job deadline exceeded");
}
static uint64_t wait_request(pending_call_t *pending) {
  for (unsigned i = 0; i < 5000; ++i) {
    uint64_t id = atomic_load(&pending->id);
    if (id)
      return id;
    sleep_ms(NULL, 1);
  }
  assert(!"capability deadline exceeded");
  return 0;
}
static void dispatch(h2_runtime_t *runtime, h2_lua_host_t *host,
                     h2_lua_job_id_t job) {
  unsigned char payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {.payload = payload,
                              .payload_capacity = sizeof(payload)};
  while (h2_runtime_poll_event(runtime, &event) == H2_PAL_OK)
    assert(h2_lua_dispatch_runtime_event(host, job, &event) == H2_PAL_OK);
}
static void test_job_results(h2_lua_host_t *host) {
  size_t invalid_size = 99;
  int invalid_present = 1;
  assert(h2_lua_job_get_result(host, H2_LUA_JOB_ID_NONE, NULL, 0,
      &invalid_size, &invalid_present) == H2_PAL_ERR_INVALID_ARG);
  assert(invalid_size == 0 && invalid_present == 0);
  assert(h2_lua_job_get_result(host, UINT32_MAX, NULL, 0,
      &invalid_size, &invalid_present) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_lua_job_get_result(NULL, 1, NULL, 0,
      &invalid_size, &invalid_present) == H2_PAL_ERR_INVALID_ARG);
  const char *scripts[] = {"return 'first', 'last'", "return 42", "return nil",
      "return", "return ''", "return {}", "return true",
      "return setmetatable({}, {__tostring=function() return 'custom' end})",
      "return 'a'..string.char(0)..'b'"};
  const char *expected[] = {"first", "42", "nil", "", "", "table: ", "true",
                            "custom", "a\0b"};
  for (size_t i = 0; i < sizeof(scripts) / sizeof(scripts[0]); ++i) {
    h2_lua_job_id_t id;
    assert(h2_lua_job_submit_text(host, NULL, "@result.lua",
        (const uint8_t *)scripts[i], strlen(scripts[i]), NULL, 0, &id) == H2_PAL_OK);
    wait_state(host, id, H2_LUA_JOB_SUCCEEDED);
    assert(status(host, id).state == H2_LUA_JOB_SUCCEEDED);
    char buffer[128] = "untouched";
    size_t size = 999;
    int present = -1;
    assert(h2_lua_job_get_result(host, id, NULL, 0, &size, &present) == H2_PAL_OK);
    assert(present == (i != 3));
    if (present) {
      assert(h2_lua_job_get_result(host, id, buffer, size, &size, &present) ==
             H2_PAL_ERR_NO_SPACE);
      assert(strcmp(buffer, "untouched") == 0);
    }
    assert(h2_lua_job_get_result(host, id, buffer, sizeof(buffer), &size,
                               &present) == H2_PAL_OK);
    if (i == 5) {
      assert(size > 7 && strncmp(buffer, expected[i], 7) == 0);
    } else {
      size_t expected_size = i == 8 ? 3 : strlen(expected[i]);
      assert(size == expected_size);
      assert(memcmp(buffer, expected[i], size) == 0 && buffer[size] == 0);
    }
    assert(status(host, id).memory_used > size);
    assert(h2_lua_job_release(host, id) == H2_PAL_OK);
    assert(h2_lua_job_get_result(host, id, buffer, sizeof(buffer), &size,
                               &present) == H2_PAL_ERR_NOT_FOUND);
    assert(size == 0 && present == 0);
  }
}

int main(void) {
  h2_runtime_t *runtime = create_runtime();
  h2_lua_host_t *host = NULL;
  pending_call_t echo = {0}, slow = {0};
  h2_lua_host_config_t cfg = {.runtime = runtime,
                              .worker_count = 1,
                              .max_jobs = 4,
                              .output_limit_bytes = 256,
                              .execution_timeout_ms = 10000,
                              .vm_memory_limit_bytes = 512u * 1024u};
  assert(h2_lua_host_create(&cfg, &host) == H2_PAL_OK);
  assert(h2_lua_capability_name_at(NULL, 0) == NULL);
  assert(h2_lua_capability_name_at(host, 0) == NULL);
  assert(h2_lua_register_capability(host, "test.echo", capability_call,
                                    capability_cancel, &echo) == H2_PAL_OK);
  assert(h2_lua_register_capability(host, "test.slow", capability_call,
                                    capability_cancel, &slow) == H2_PAL_OK);
  assert(strcmp(h2_lua_capability_name_at(host, 0), "test.echo") == 0);
  assert(strcmp(h2_lua_capability_name_at(host, 1), "test.slow") == 0);
  assert(h2_lua_capability_name_at(host, 2) == NULL);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  test_job_results(host);
  const char *failures[] = {"return string.rep('x',257)", "error('failed')",
      "return setmetatable({}, {__tostring=function() error('conversion') end})"};
  for (size_t i = 0; i < 3; ++i) {
    h2_lua_job_id_t id = submit(host, failures[i]);
    wait_state(host, id, H2_LUA_JOB_FAILED);
    if (i == 0) assert(strcmp(status(host, id).message,
                              "H2_LUA_VM_OUTPUT_TOO_LARGE") == 0);
    size_t size = 99;
    int present = 1;
    assert(h2_lua_job_get_result(host, id, NULL, 0, &size, &present) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(size == 0 && present == 0);
    assert(h2_lua_job_release(host, id) == H2_PAL_OK);
  }
  h2_lua_job_id_t boundary = submit(host, "return string.rep('x',256)");
  wait_state(host, boundary, H2_LUA_JOB_SUCCEEDED);
  char result_buffer[257];
  size_t result_size;
  int has_result;
  assert(h2_lua_job_get_result(host, boundary, result_buffer,
      sizeof(result_buffer), &result_size, &has_result) == H2_PAL_OK);
  assert(result_size == 256 && has_result);
  for (size_t i = 0; i < result_size; ++i) assert(result_buffer[i] == 'x');
  assert(result_buffer[result_size] == 0);
  assert(h2_lua_job_release(host, boundary) == H2_PAL_OK);
  assert(h2_lua_register_capability(host, "late", capability_call, NULL,
                                    &echo) == H2_PAL_ERR_INVALID_STATE);
  assert(strcmp(h2_lua_capability_name_at(host, 1), "test.slow") == 0);

  h2_lua_job_id_t job = submit(
      host, "local ok,out,err=require('capability').call('test.echo','hello');"
            "assert(ok and out=='hello' and err==nil)");
  uint64_t request = wait_request(&echo);
  wait_state(host, job, H2_LUA_JOB_WAITING);
  assert(strcmp(echo.input, "hello") == 0);
  assert(h2_lua_capability_complete(host, request, H2_PAL_OK, echo.input,
                                    NULL) == H2_PAL_OK);
  wait_state(host, job, H2_LUA_JOB_SUCCEEDED);
  assert(h2_lua_job_release(host, job) == H2_PAL_OK);

  job = submit(host, "local d=require('display');d.present();"
                     "d.fill_rect(3,5,7,9,'red');d.present();"
                     "local t=require('lcd_touch');t.poll()");
  wait_state(host, job, H2_LUA_JOB_SUCCEEDED);
  assert(display.draws == 2 && display.presents == 2);
  assert(display.rect.x == 3 && display.rect.y == 5);
  assert(display.rect.width == 7 && display.rect.height == 9);
  for (int y = 0; y < 240; ++y)
    for (int x = 0; x < 240; ++x)
      assert(display.pixels[y * 240 + x] ==
             ((x >= 3 && x < 10 && y >= 5 && y < 14) ? 0xf800 : 0));
  assert(h2_lua_job_release(host, job) == H2_PAL_OK);

  atomic_store(&echo.id, 0);
  job = submit(
      host,
      "local r=require('runtime');local pressed=false;"
      "r.components.on(1,r.event.BUTTON_DOWN,function(e) pressed=true end);"
      "assert(require('capability').call('test.echo','ready'));"
      "while not pressed do r.sleep(1) end");
  request = wait_request(&echo); /* Subscription exists before either edge. */
  wait_state(host, job, H2_LUA_JOB_WAITING);
  assert(h2_runtime_button_push_edge(runtime, 2, H2_RUNTIME_BUTTON_EDGE_DOWN) ==
         H2_PAL_OK);
  dispatch(runtime, host, job);
  assert(status(host, job).state == H2_LUA_JOB_WAITING);
  assert(h2_lua_capability_complete(host, request, H2_PAL_OK, "ready", NULL) ==
         H2_PAL_OK);
  sleep_ms(NULL, 10);
  assert(status(host, job).state < H2_LUA_JOB_SUCCEEDED);
  assert(h2_runtime_button_push_edge(runtime, 1, H2_RUNTIME_BUTTON_EDGE_DOWN) ==
         H2_PAL_OK);
  for (unsigned i = 0;
       i < 5000 && status(host, job).state < H2_LUA_JOB_SUCCEEDED; ++i) {
    dispatch(runtime, host, job);
    sleep_ms(NULL, 1);
  }
  wait_state(host, job, H2_LUA_JOB_SUCCEEDED);
  assert(h2_lua_job_release(host, job) == H2_PAL_OK);

  job = submit(host, "require('capability').call('test.slow','cancel me')");
  request = wait_request(&slow);
  wait_state(host, job, H2_LUA_JOB_WAITING);
  assert(h2_lua_job_get_result(host, job, NULL, 0, &result_size, &has_result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(result_size == 0 && has_result == 0);
  assert(h2_lua_job_cancel(host, job) == H2_PAL_OK);
  wait_state(host, job, H2_LUA_JOB_CANCELLED);
  assert(h2_lua_job_get_result(host, job, NULL, 0, &result_size, &has_result) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(result_size == 0 && has_result == 0);
  assert(h2_lua_job_release(host, job) == H2_PAL_OK);
  for (unsigned i = 0; i < 5000 && atomic_load(&slow.cancels) == 0; ++i)
    sleep_ms(NULL, 1);
  assert(atomic_load(&slow.cancels) == 1);
  assert(h2_lua_capability_complete(host, request, H2_PAL_OK, "late", NULL) !=
         H2_PAL_OK);
  assert(h2_lua_host_stop(host) == H2_PAL_OK);
  assert(h2_lua_host_join(host) == H2_PAL_OK);
  h2_lua_host_destroy(host);
  h2_runtime_deinit(runtime);
  puts("PASS: PAL embedding, async echo, display, ok/back input, cancellation, "
       "frozen registry, job results");
  return 0;
}
