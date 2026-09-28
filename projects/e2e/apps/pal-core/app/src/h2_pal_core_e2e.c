#include "h2_pal_core_e2e.h"
#include "h2_pal_core_e2e_task_names.h"
#include "h2_pal_core_extended.h"

#include <stdio.h>
#include <string.h>

struct h2_pal_core_e2e_owner {
  h2_runtime_t *runtime;
  const h2_pal_core_e2e_config_t *config;
  uint32_t timeout_ms;
  h2_pal_timer_t *timer;
  h2_pal_queue_t *queue;
  int queue_closed;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *condition;
  h2_pal_semaphore_t *semaphore;
  h2_pal_semaphore_t *ready;
  h2_pal_task_t *task;
  h2_pal_result_t worker_result;
  int worker_value;
  int worker_mode;
  h2_pal_system_event_subscription_t *subscriptions[2];
  int events_initialized;
  h2_pal_netif_default_changed_t event_payload;
  h2_pal_system_event_t event;
  int wall_restore_pending;
  uint64_t restore_wall;
  uint64_t restore_started;
  h2_pal_core_extended_t *extension;
};

typedef h2_pal_core_e2e_owner_t owner_t;

static const char *const case_ids[H2_PAL_CORE_E2E_CASE_COUNT] = {
#define H2_PAL_CORE_CASE(id) id,
#include "h2_pal_core_cases.inc"
#undef H2_PAL_CORE_CASE
};

#define TRY(expr)                                                              \
  do {                                                                         \
    rc = (h2_pal_result_t)(expr);                                              \
    if (rc != H2_PAL_OK)                                                       \
      return rc;                                                               \
  } while (0)
#define REQUIRE(expr)                                                          \
  do {                                                                         \
    if (!(expr))                                                               \
      return H2_PAL_ERR_INVALID_STATE;                                         \
  } while (0)

static h2_pal_result_t memory_case(h2_runtime_t *r) {
  uint8_t *p = h2_pal_mem_alloc(r->mem, 31u);
  uint8_t *q = h2_pal_mem_realloc(r->mem, NULL, 13u);
  h2_pal_result_t rc = H2_PAL_OK;
  if (p == NULL || q == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  memset(p, 0xa5, 31u);
  memset(q, 0x3c, 13u);
  uint8_t *grown = h2_pal_mem_realloc(r->mem, p, 97u);
  if (grown == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  p = grown;
  for (size_t i = 0; i < 31u; ++i)
    if (p[i] != 0xa5u)
      rc = H2_PAL_ERR_INVALID_STATE;
  for (size_t i = 0; i < 13u; ++i)
    if (q[i] != 0x3cu)
      rc = H2_PAL_ERR_INVALID_STATE;
  uint8_t *shrunk = h2_pal_mem_realloc(r->mem, p, 7u);
  if (shrunk == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  p = shrunk;
  for (size_t i = 0; i < 7u; ++i)
    if (p[i] != 0xa5u)
      rc = H2_PAL_ERR_INVALID_STATE;
done:
  h2_pal_mem_free(r->mem, q);
  h2_pal_mem_free(r->mem, p);
  h2_pal_mem_free(r->mem, NULL);
  return rc;
}

static h2_pal_result_t log_case(owner_t *o) {
  if (o->config->observe_log == NULL)
    return H2_PAL_ERR_UNAVAILABLE;
  h2_pal_result_t rc;
  for (int level = H2_PAL_LOG_DEBUG; level <= H2_PAL_LOG_ERROR; ++level) {
    char message[H2_PAL_LOG_MESSAGE_MAX];
    memset(message, 'x', sizeof(message));
    memcpy(message, "pal-core-output-", 16u);
    message[16] = (char)('0' + level);
    message[sizeof(message) - 1u] = '\0';
    TRY(h2_pal_log_write(o->runtime->log, (h2_pal_log_level_t)level, "pal-core",
                         message));
    TRY(o->config->observe_log(o->config->observer_user,
                               (h2_pal_log_level_t)level, "pal-core", message));
  }
  REQUIRE(h2_pal_log_write(o->runtime->log, H2_PAL_LOG_INFO, "pal-core",
                           NULL) == H2_PAL_ERR_INVALID_ARG);
  return H2_PAL_OK;
}

static h2_pal_result_t observe(owner_t *o, uint64_t *us) {
  return o->config->observe_monotonic_us == NULL
             ? H2_PAL_ERR_UNAVAILABLE
             : o->config->observe_monotonic_us(o->config->observer_user, us);
}

static h2_pal_result_t time_case(owner_t *o) {
  h2_pal_result_t rc;
  uint64_t external_before, external_after, ms_before, ms_after, us_before,
      us_after;
  TRY(observe(o, &external_before));
  TRY(h2_pal_time_get_monotonic_ms(o->runtime->time, &ms_before));
  TRY(h2_pal_time_get_monotonic_us(o->runtime->time, &us_before));
  TRY(h2_pal_time_sleep_ms(o->runtime->time, 20u));
  TRY(h2_pal_time_get_monotonic_us(o->runtime->time, &us_after));
  TRY(h2_pal_time_get_monotonic_ms(o->runtime->time, &ms_after));
  TRY(observe(o, &external_after));
  REQUIRE(external_after >= external_before + 19000u);
  REQUIRE(ms_after >= ms_before + 19u && us_after >= us_before + 19000u);
  /* A coarse microsecond clock is valid; compare domains, never require that
   * microseconds have more resolution than milliseconds. */
  REQUIRE(us_before / 1000u >= ms_before && us_after / 1000u <= ms_after);
  REQUIRE(us_after / 1000u - ms_before <=
          external_after / 1000u - external_before / 1000u + 2u);
  return h2_pal_time_sleep_ms(o->runtime->time, 0u);
}

static h2_pal_result_t wall_read_case(owner_t *o) {
  h2_pal_result_t rc;
  h2_pal_time_wall_status_t status;
  TRY(h2_pal_time_get_wall_status(o->runtime->time, &status));
  uint64_t wall = UINT64_MAX;
  rc = h2_pal_time_get_wall_ms(o->runtime->time, &wall);
  if (!status.valid) {
    REQUIRE(rc == H2_PAL_TIME_ERR_UNCALIBRATED && wall == 0u);
    return H2_PAL_OK;
  }
  REQUIRE(rc == H2_PAL_OK && wall != 0u);
  REQUIRE(status.source >= H2_PAL_TIME_WALL_SOURCE_UNKNOWN &&
          status.source <= H2_PAL_TIME_WALL_SOURCE_SERVER_ALIGNED);
  return H2_PAL_OK;
}

static h2_pal_result_t wall_set_case(owner_t *o) {
  if (!o->config->allow_wall_set)
    return H2_PAL_ERR_UNAVAILABLE;
  h2_pal_result_t rc;
  uint64_t original = 0, before = 0, after = 0, wall = 0, ext_before = 0, ext_after = 0;
  TRY(h2_pal_time_get_wall_ms(o->runtime->time, &original));
  TRY(observe(o, &ext_before));
  TRY(h2_pal_time_get_monotonic_us(o->runtime->time, &before));
  const uint64_t target = UINT64_C(1700000000000);
  TRY(h2_pal_time_set_wall_ms(o->runtime->time, target));
  o->wall_restore_pending = 1;
  o->restore_wall = original;
  o->restore_started = ext_before;
  rc = h2_pal_time_get_wall_ms(o->runtime->time, &wall);
  if (rc == H2_PAL_OK && (wall < target || wall - target > o->timeout_ms))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    rc = h2_pal_time_get_monotonic_us(o->runtime->time, &after);
  h2_pal_result_t clock_rc = observe(o, &ext_after);
  if (rc == H2_PAL_OK)
    rc = clock_rc;
  if (rc == H2_PAL_OK &&
      (after < before || after - before > ext_after - ext_before + 2000u))
    rc = H2_PAL_ERR_INVALID_STATE;
  /* Always restore after a successful set, including assertion/read failure. */
  h2_pal_result_t restore = h2_pal_time_set_wall_ms(
      o->runtime->time,
      original +
          (clock_rc == H2_PAL_OK ? (ext_after - ext_before) / 1000u : 0u));
  if (restore == H2_PAL_OK)
    o->wall_restore_pending = 0;
  return restore != H2_PAL_OK ? restore : rc;
}

static h2_pal_result_t make_queue(owner_t *o, size_t capacity) {
  h2_pal_queue_config_t cfg = {.name = "pal-core",
                               .item_size = sizeof(int),
                               .item_count = capacity,
                               .allocator = o->runtime->mem};
  return (h2_pal_result_t)h2_pal_queue_create(o->runtime->queue, &cfg,
                                              &o->queue);
}

static h2_pal_result_t queue_case(owner_t *o) {
  if (o->config->queue_latest == H2_PAL_CORE_QUEUE_LATEST_UNSPECIFIED)
    return H2_PAL_ERR_UNAVAILABLE;
  h2_pal_result_t rc;
  TRY(make_queue(o, 2u));
  int a = 11, b = 22, c = 33, value = 0;
  REQUIRE(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, 0u) ==
          H2_PAL_ERR_TIMEOUT);
  TRY(h2_pal_queue_send(o->runtime->queue, o->queue, &a, 0u));
  TRY(h2_pal_queue_send(o->runtime->queue, o->queue, &b, 0u));
  REQUIRE(h2_pal_queue_send(o->runtime->queue, o->queue, &c, 5u) ==
          H2_PAL_ERR_TIMEOUT);
  int native_latest =
      o->config->queue_latest == H2_PAL_CORE_QUEUE_LATEST_REPLACE;
  rc = (h2_pal_result_t)h2_pal_queue_send_latest(o->runtime->queue, o->queue,
                                                 &c);
  REQUIRE(rc == (native_latest ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT));
  TRY(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, 0u));
  REQUIRE(value == (native_latest ? b : a));
  TRY(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, 0u));
  REQUIRE(value == (native_latest ? c : b));
  TRY(h2_pal_queue_send_latest(o->runtime->queue, o->queue, &a));
  TRY(h2_pal_queue_reset(o->runtime->queue, o->queue));
  REQUIRE(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, 5u) ==
          H2_PAL_ERR_TIMEOUT);
  TRY(h2_pal_queue_send(o->runtime->queue, o->queue, &a, 0u));
  TRY(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, 0u));
  REQUIRE(value == a);
  TRY(h2_pal_queue_close(o->runtime->queue, o->queue));
  o->queue_closed = 1;
  REQUIRE(h2_pal_queue_send(o->runtime->queue, o->queue, &a, 0u) ==
          H2_PAL_ERR_CLOSED);
  REQUIRE(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, 0u) ==
          H2_PAL_ERR_CLOSED);
  return H2_PAL_OK;
}

static h2_pal_result_t mutex_case(owner_t *o) {
  const h2_pal_mutex_config_t cfg = {.name = "pal-core-recursive",
                                     .allocator = o->runtime->mem,
                                     .flags = H2_PAL_MUTEX_FLAG_RECURSIVE};
  h2_pal_result_t rc;
  TRY(h2_pal_mutex_create(o->runtime->sync, &cfg, &o->mutex));
  TRY(h2_pal_mutex_lock(o->runtime->sync, o->mutex));
  rc = h2_pal_mutex_try_lock(o->runtime->sync, o->mutex);
  if (rc == H2_PAL_OK)
    rc = h2_pal_mutex_unlock(o->runtime->sync, o->mutex);
  h2_pal_result_t unlock = h2_pal_mutex_unlock(o->runtime->sync, o->mutex);
  return rc == H2_PAL_OK ? unlock : rc;
}

static h2_pal_result_t make_semaphore(owner_t *o) {
  const h2_pal_semaphore_config_t cfg = {.name = "pal-core-sem",
                                         .allocator = o->runtime->mem,
                                         .initial_count = 0u,
                                         .max_count = 2u};
  return h2_pal_semaphore_create(o->runtime->sync, &cfg, &o->semaphore);
}

static h2_pal_result_t semaphore_case(owner_t *o) {
  h2_pal_result_t rc;
  TRY(make_semaphore(o));
  REQUIRE(h2_pal_semaphore_take(o->runtime->sync, o->semaphore, 0u) ==
          H2_PAL_ERR_TIMEOUT);
  TRY(h2_pal_semaphore_give(o->runtime->sync, o->semaphore));
  TRY(h2_pal_semaphore_give(o->runtime->sync, o->semaphore));
  REQUIRE(h2_pal_semaphore_give(o->runtime->sync, o->semaphore) != H2_PAL_OK);
  TRY(h2_pal_semaphore_take(o->runtime->sync, o->semaphore, 0u));
  TRY(h2_pal_semaphore_take(o->runtime->sync, o->semaphore, 0u));
  REQUIRE(h2_pal_semaphore_take(o->runtime->sync, o->semaphore, 5u) ==
          H2_PAL_ERR_TIMEOUT);
  return H2_PAL_OK;
}

static h2_pal_result_t condition_case(owner_t *o) {
  h2_pal_result_t rc;
  const h2_pal_mutex_config_t mutex_cfg = {.name = "pal-core-cond",
                                           .allocator = o->runtime->mem,
                                           .flags = H2_PAL_MUTEX_FLAG_NONE};
  const h2_pal_cond_config_t cond_cfg = {.name = "pal-core-cond",
                                         .allocator = o->runtime->mem};
  TRY(h2_pal_mutex_create(o->runtime->sync, &mutex_cfg, &o->mutex));
  TRY(h2_pal_cond_create(o->runtime->sync, &cond_cfg, &o->condition));
  TRY(h2_pal_cond_signal(o->runtime->sync, o->condition));
  TRY(h2_pal_cond_broadcast(o->runtime->sync, o->condition));
  TRY(h2_pal_mutex_lock(o->runtime->sync, o->mutex));
  /* Spurious success is allowed. Keep waiting for the unchanged predicate to
   * reach a real timeout, bounded by the independent observer and attempts. */
  uint64_t start = 0u, now = 0u;
  rc = observe(o, &start);
  for (size_t i = 0; rc == H2_PAL_OK && i < 100u; ++i) {
    rc = h2_pal_cond_wait(o->runtime->sync, o->condition, o->mutex, 5u);
    if (rc == H2_PAL_ERR_TIMEOUT) {
      rc = H2_PAL_OK;
      break;
    }
    if (rc == H2_PAL_OK)
      rc = observe(o, &now);
    if (rc == H2_PAL_OK &&
        (now - start >= (uint64_t)o->timeout_ms * 1000u || i == 99u))
      rc = H2_PAL_ERR_TIMEOUT;
  }
  const h2_pal_result_t unlock =
      h2_pal_mutex_unlock(o->runtime->sync, o->mutex);
  return rc == H2_PAL_OK ? unlock : rc;
}

static void worker(void *user) {
  owner_t *o = user;
  o->worker_result = h2_pal_semaphore_give(o->runtime->sync, o->ready);
  if (o->worker_result != H2_PAL_OK)
    return;
  if (o->worker_mode == 1) {
    o->worker_result = (h2_pal_result_t)h2_pal_queue_recv(
        o->runtime->queue, o->queue, &o->worker_value, o->timeout_ms);
  } else {
    o->worker_result =
        h2_pal_semaphore_take(o->runtime->sync, o->semaphore, o->timeout_ms);
  }
}

static h2_pal_result_t join(owner_t *o) {
  h2_pal_result_t rc;
  uint64_t start, now;
  TRY(observe(o, &start));
  for (uint32_t attempts = 0; attempts <= o->timeout_ms; ++attempts) {
    rc = h2_pal_task_join(o->runtime->task, o->task);
    if (rc == H2_PAL_OK) {
      o->task = NULL;
      return rc;
    }
    if (rc != H2_PAL_ERR_BUSY)
      return rc;
    TRY(observe(o, &now));
    if (now - start >= (uint64_t)o->timeout_ms * 1000u)
      return H2_PAL_ERR_TIMEOUT;
    TRY(h2_pal_time_sleep_ms(o->runtime->time, 1u));
  }
  return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t wake_case(owner_t *o, int queue) {
  h2_pal_result_t rc;
  if (queue) {
    TRY(make_queue(o, 1u));
  } else {
    TRY(make_semaphore(o));
  }
  const h2_pal_semaphore_config_t ready_cfg = {.name = "pal-core-ready",
                                               .allocator = o->runtime->mem,
                                               .initial_count = 0u,
                                               .max_count = 1u};
  TRY(h2_pal_semaphore_create(o->runtime->sync, &ready_cfg, &o->ready));
  o->worker_mode = queue ? 1 : 2;
  o->worker_result = H2_PAL_ERR_INVALID_STATE;
  const h2_pal_task_options_t options = {.name =
                                             h2_pal_core_e2e_queue_task_name};
  TRY(h2_pal_task_start(o->runtime->task, &options, worker, o, &o->task));
  TRY(h2_pal_semaphore_take(o->runtime->sync, o->ready, o->timeout_ms));
  TRY(h2_pal_time_sleep_ms(o->runtime->time, 10u));
  uint64_t wake_at, finished_at;
  TRY(observe(o, &wake_at));
  if (queue) {
    TRY(h2_pal_queue_close(o->runtime->queue, o->queue));
    o->queue_closed = 1;
  } else {
    TRY(h2_pal_semaphore_give(o->runtime->sync, o->semaphore));
  }
  TRY(join(o));
  TRY(observe(o, &finished_at));
  REQUIRE(finished_at - wake_at < (uint64_t)o->timeout_ms * 500u);
  REQUIRE(o->worker_result == (queue ? H2_PAL_ERR_CLOSED : H2_PAL_OK));
  return H2_PAL_OK;
}

static void timer_callback(void *user, h2_pal_timer_t *timer) {
  (void)timer;
  owner_t *o = user;
  const int message = 73;
  (void)h2_pal_queue_send(o->runtime->queue, o->queue, &message, 0u);
}

static h2_pal_result_t timer_case(owner_t *o) {
  h2_pal_result_t rc;
  TRY(make_queue(o, 16u));
  const h2_pal_timer_config_t cfg = {.name = "pal-core-repeat",
                                     .period_ms = 10u,
                                     .flags = H2_PAL_TIMER_FLAG_REPEAT,
                                     .cb = timer_callback,
                                     .cb_user = o};
  TRY(h2_pal_timer_create(o->runtime->timer, &cfg, &o->timer));
  int running = -1, value = 0;
  TRY(h2_pal_timer_is_running(o->runtime->timer, o->timer, &running));
  REQUIRE(!running);
  TRY(h2_pal_timer_set_period_ms(o->runtime->timer, o->timer, 5u));
  REQUIRE(h2_pal_timer_set_period_ms(o->runtime->timer, o->timer, 0u) ==
          H2_PAL_ERR_INVALID_ARG);
  TRY(h2_pal_timer_start(o->runtime->timer, o->timer));
  TRY(h2_pal_timer_is_running(o->runtime->timer, o->timer, &running));
  REQUIRE(running);
  for (int i = 0; i < 3; ++i) {
    TRY(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, o->timeout_ms));
    REQUIRE(value == 73);
  }
  TRY(h2_pal_timer_stop(o->runtime->timer, o->timer));
  TRY(h2_pal_timer_is_running(o->runtime->timer, o->timer, &running));
  REQUIRE(!running);
  TRY(h2_pal_time_sleep_ms(o->runtime->time, 20u));
  TRY(h2_pal_queue_reset(o->runtime->queue, o->queue));
  REQUIRE(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, 20u) ==
          H2_PAL_ERR_TIMEOUT);
  TRY(h2_pal_timer_reset(o->runtime->timer, o->timer));
  TRY(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, o->timeout_ms));
  REQUIRE(value == 73);
  return H2_PAL_OK;
}

static h2_pal_result_t oneshot_case(owner_t *o) {
  h2_pal_result_t rc;
  TRY(make_queue(o, 8u));
  const h2_pal_timer_config_t cfg = {.name = "pal-core-oneshot",
                                     .period_ms = 10u,
                                     .flags = H2_PAL_TIMER_FLAG_AUTO_START,
                                     .cb = timer_callback,
                                     .cb_user = o};
  TRY(h2_pal_timer_create(o->runtime->timer, &cfg, &o->timer));
  int value = 0, running = -1;
  TRY(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, o->timeout_ms));
  REQUIRE(value == 73);
  REQUIRE(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, 20u) ==
          H2_PAL_ERR_TIMEOUT);
  TRY(h2_pal_timer_is_running(o->runtime->timer, o->timer, &running));
  REQUIRE(!running);
  TRY(h2_pal_timer_start(o->runtime->timer, o->timer));
  TRY(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, o->timeout_ms));
  REQUIRE(value == 73);
  return H2_PAL_OK;
}

static int event_handler(void *user, const h2_pal_system_event_t *event) {
  owner_t *o = user;
  int message = 0;
  if (event->source_id == 0x50414cu && event->timestamp_ms == 123u &&
      event->type == H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED &&
      event->payload_size == sizeof(h2_pal_netif_default_changed_t)) {
    const h2_pal_netif_default_changed_t *change = event->payload;
    if (change->current_valid &&
        change->current.type == H2_PAL_NETIF_REF_NAME &&
        strcmp(change->current.name, "e2e0") == 0)
      message = 91;
  }
  return h2_pal_queue_send(o->runtime->queue, o->queue, &message, 0u);
}

static int other_event_handler(void *user, const h2_pal_system_event_t *event) {
  (void)event;
  owner_t *o = user;
  int message = -1;
  return h2_pal_queue_send(o->runtime->queue, o->queue, &message, 0u);
}

static h2_pal_result_t events_case(owner_t *o) {
  const h2_pal_system_event_api_t *api = o->config->event_fixture;
  if (api == NULL)
    return H2_PAL_ERR_UNAVAILABLE;
  if (api->vtable == NULL || api->vtable->init == NULL ||
      api->vtable->deinit == NULL || api->vtable->unsubscribe == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_result_t rc;
  TRY(make_queue(o, 8u));
  for (int cycle = 0; cycle < 2; ++cycle) {
    TRY(h2_pal_system_event_init(api));
    o->events_initialized = 1;
    TRY(h2_pal_system_event_subscribe(
        api, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED, event_handler, o,
        &o->subscriptions[0]));
    TRY(h2_pal_system_event_subscribe(
        api, H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED, other_event_handler,
        o, &o->subscriptions[1]));
    memset(&o->event_payload, 0, sizeof(o->event_payload));
    o->event_payload.current_valid = 1;
    o->event_payload.current.type = H2_PAL_NETIF_REF_NAME;
    memcpy(o->event_payload.current.name, "e2e0", sizeof("e2e0"));
    o->event = (h2_pal_system_event_t){
        .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
        .source_id = 0x50414cu,
        .timestamp_ms = 123u,
        .payload = &o->event_payload,
        .payload_size = sizeof(o->event_payload)};
    TRY(h2_pal_system_event_post(api, &o->event, o->timeout_ms));
    int value = 0;
    TRY(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, o->timeout_ms));
    REQUIRE(value == 91);
    h2_pal_system_event_unsubscribe(api, o->subscriptions[0]);
    o->subscriptions[0] = NULL;
    TRY(h2_pal_system_event_post(api, &o->event, o->timeout_ms));
    REQUIRE(h2_pal_queue_recv(o->runtime->queue, o->queue, &value, 20u) ==
            H2_PAL_ERR_TIMEOUT);
    h2_pal_system_event_unsubscribe(api, o->subscriptions[1]);
    o->subscriptions[1] = NULL;
    h2_pal_system_event_deinit(api);
    o->events_initialized = 0;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t firmware_case(owner_t *o) {
  h2_pal_result_t rc;
  h2_pal_firmware_info_t first, second;
  TRY(h2_pal_firmware_info_get_current(o->runtime->firmware_info, &first));
  if (o->config->firmware_version == NULL ||
      o->config->firmware_version[0] == '\0')
    return H2_PAL_ERR_UNAVAILABLE;
  TRY(h2_pal_firmware_info_get_current(o->runtime->firmware_info, &second));
  REQUIRE(strcmp(first.version, o->config->firmware_version) == 0);
  REQUIRE(strcmp(first.version, second.version) == 0);
  return H2_PAL_OK;
}

static h2_pal_result_t release(owner_t *o) {
  h2_pal_result_t rc;
  TRY(h2_pal_core_extended_cleanup(&o->extension));
  if (o->wall_restore_pending) {
    uint64_t now;
    TRY(observe(o, &now));
    TRY(h2_pal_time_set_wall_ms(o->runtime->time,
                                o->restore_wall +
                                    (now - o->restore_started) / 1000u));
    o->wall_restore_pending = 0;
  }
  if (o->task != NULL) {
    if (o->queue != NULL && !o->queue_closed) {
      o->queue_closed =
          h2_pal_queue_close(o->runtime->queue, o->queue) == H2_PAL_OK;
    }
    if (o->semaphore != NULL)
      (void)h2_pal_semaphore_give(o->runtime->sync, o->semaphore);
    TRY(join(o));
  }
  if (o->timer != NULL) {
    TRY(h2_pal_timer_destroy(o->runtime->timer, o->timer));
    o->timer = NULL;
  }
  if (o->events_initialized) {
    for (size_t i = 0; i < 2; ++i) {
      h2_pal_system_event_unsubscribe(o->config->event_fixture,
                                      o->subscriptions[i]);
      o->subscriptions[i] = NULL;
    }
    h2_pal_system_event_deinit(o->config->event_fixture);
    o->events_initialized = 0;
  }
  if (o->queue != NULL) {
    if (!o->queue_closed) {
      TRY(h2_pal_queue_close(o->runtime->queue, o->queue));
    }
    h2_pal_queue_destroy(o->runtime->queue, o->queue);
    o->queue = NULL;
    o->queue_closed = 0;
  }
  if (o->condition != NULL) {
    TRY(h2_pal_cond_destroy(o->runtime->sync, o->condition));
    o->condition = NULL;
  }
  if (o->mutex != NULL) {
    TRY(h2_pal_mutex_destroy(o->runtime->sync, o->mutex));
    o->mutex = NULL;
  }
  if (o->ready != NULL) {
    TRY(h2_pal_semaphore_destroy(o->runtime->sync, o->ready));
    o->ready = NULL;
  }
  if (o->semaphore != NULL) {
    TRY(h2_pal_semaphore_destroy(o->runtime->sync, o->semaphore));
    o->semaphore = NULL;
  }
  return H2_PAL_OK;
}

#define HAS(api, op)                                                           \
  ((api) != NULL && (api)->vtable != NULL && (api)->vtable->op != NULL)
static int ready(h2_runtime_t *r, const h2_pal_core_e2e_config_t *cfg,
                 size_t index) {
  if (index == 0u || index == 1u || index == 3u || index == 4u || index == 16u)
    return 1;
  if (!HAS(r->time, get_monotonic_ms) || !HAS(r->time, sleep_ms) ||
      cfg->observe_monotonic_us == NULL)
    return 0;
  if (index == 2u)
    return HAS(r->time, get_monotonic_us);
  if (index == 5u || index == 6u) {
    if (!HAS(r->timer, create) || !HAS(r->timer, destroy) ||
        !HAS(r->timer, start) || !HAS(r->timer, stop) ||
        !HAS(r->timer, reset) || !HAS(r->timer, set_period_ms) ||
        !HAS(r->timer, is_running))
      return 0;
  }
  if (index == 7u || index == 8u || index == 12u || index == 14u ||
      index >= 17u) {
    if (!HAS(r->task, start) || !HAS(r->task, join))
      return 0;
  }
  if (index == 5u || index == 6u || index == 8u || index == 9u ||
      index == 14u || index == 15u || index == 17u) {
    if (!HAS(r->queue, create) || !HAS(r->queue, destroy) ||
        !HAS(r->queue, close) || !HAS(r->queue, send) || !HAS(r->queue, recv) ||
        !HAS(r->queue, reset))
      return 0;
  }
  if (index >= 10u && index <= 14u) {
    if (!HAS(r->sync, create_mutex) || !HAS(r->sync, destroy_mutex) ||
        !HAS(r->sync, lock_mutex) || !HAS(r->sync, unlock_mutex))
      return 0;
  }
  if (index == 10u && !HAS(r->sync, try_lock_mutex))
    return 0;
  if (index == 11u || index == 17u || index == 18u) {
    if (!HAS(r->sync, create_semaphore) || !HAS(r->sync, destroy_semaphore) ||
        !HAS(r->sync, take_semaphore) || !HAS(r->sync, give_semaphore))
      return 0;
  }
  if (index == 12u || index == 13u || index == 14u) {
    if (!HAS(r->sync, create_cond) || !HAS(r->sync, destroy_cond) ||
        !HAS(r->sync, wait_cond) || !HAS(r->sync, signal_cond) ||
        !HAS(r->sync, broadcast_cond))
      return 0;
  }
  return 1;
}
#undef HAS

static void summarize(h2_pal_core_e2e_result_t *result) {
  result->passed = result->failed = result->blocked = result->not_run = 0;
  for (size_t i = 0; i < H2_PAL_CORE_E2E_CASE_COUNT; ++i) {
    switch (result->cases[i].status) {
    case H2_PAL_CORE_E2E_PASS:
      ++result->passed;
      break;
    case H2_PAL_CORE_E2E_FAIL:
      ++result->failed;
      break;
    case H2_PAL_CORE_E2E_BLOCKED:
      ++result->blocked;
      break;
    default:
      ++result->not_run;
      break;
    }
  }
  result->complete = result->not_run == 0;
  result->qualified = result->passed == H2_PAL_CORE_E2E_CASE_COUNT &&
                      result->cleanup_result == H2_PAL_OK &&
                      result->baseline.retained_cleanup == NULL &&
                      result->retained_cleanup == NULL;
}

h2_pal_result_t h2_pal_core_e2e_cleanup(h2_runtime_t *runtime,
                                        h2_pal_core_e2e_result_t *result) {
  if (runtime == NULL || result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = h2_pal_core_baseline_cleanup(runtime, &result->baseline);
  if (rc != H2_PAL_OK)
    return rc;
  if (result->retained_cleanup != NULL) {
    rc = release(result->retained_cleanup);
    if (rc != H2_PAL_OK)
      return rc;
    h2_pal_mem_free(runtime->mem, result->retained_cleanup);
    result->retained_cleanup = NULL;
  }
  summarize(result);
  return H2_PAL_OK;
}

h2_pal_result_t h2_pal_core_e2e_run(h2_runtime_t *runtime,
                                    const h2_pal_core_e2e_config_t *config,
                                    h2_pal_core_e2e_result_t *result) {
  if (result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(result, 0, sizeof(*result));
  for (size_t i = 0; i < H2_PAL_CORE_E2E_CASE_COUNT; ++i)
    result->cases[i].id = case_ids[i];
  if (runtime == NULL || config == NULL ||
      (unsigned)config->queue_latest > H2_PAL_CORE_QUEUE_LATEST_FALLBACK ||
      (config->timeout_ms != 0u &&
       (config->timeout_ms < 100u || config->timeout_ms > 60000u))) {
    summarize(result);
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (runtime->mem == NULL || runtime->mem->vtable == NULL ||
      runtime->mem->vtable->alloc == NULL ||
      runtime->mem->vtable->realloc == NULL ||
      runtime->mem->vtable->free == NULL) {
    for (size_t i = 0; i < H2_PAL_CORE_E2E_CASE_COUNT; ++i) {
      result->cases[i].status = H2_PAL_CORE_E2E_BLOCKED;
      result->cases[i].result = H2_PAL_ERR_UNSUPPORTED;
    }
    summarize(result);
    return H2_PAL_ERR_UNSUPPORTED;
  }
  h2_pal_core_resources_t resources_before = {0};
  h2_pal_result_t resource_rc =
      config->observe_resources == NULL
          ? H2_PAL_ERR_UNAVAILABLE
          : config->observe_resources(config->observer_user, &resources_before);
  owner_t *o = h2_pal_mem_alloc(runtime->mem, sizeof(*o));
  if (o == NULL) {
    summarize(result);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(o, 0, sizeof(*o));
  o->runtime = runtime;
  o->config = config;
  o->timeout_ms = config->timeout_ms == 0u ? 2000u : config->timeout_ms;
  for (size_t i = 0; i + 1u < H2_PAL_CORE_E2E_CASE_COUNT; ++i) {
    if (config->case_begin != NULL)
      config->case_begin(config->observer_user, case_ids[i]);
    h2_pal_result_t rc = H2_PAL_ERR_UNSUPPORTED;
    if (i >= 19u || ready(runtime, config, i))
      switch (i) {
      case 0:
        rc = memory_case(runtime);
        break;
      case 1:
        rc = log_case(o);
        break;
      case 2:
        rc = time_case(o);
        break;
      case 3:
        rc = wall_read_case(o);
        break;
      case 4:
        rc = wall_set_case(o);
        break;
      case 5:
        rc = oneshot_case(o);
        break;
      case 6:
        rc = timer_case(o);
        break;
      case 7:
        rc = h2_pal_core_baseline_task(runtime, &result->baseline);
        break;
      case 8:
        rc = h2_pal_core_baseline_queue(runtime, &result->baseline);
        break;
      case 9:
        rc = queue_case(o);
        break;
      case 10:
        rc = mutex_case(o);
        break;
      case 11:
        rc = semaphore_case(o);
        break;
      case 12:
        rc = h2_pal_core_baseline_condition(runtime, &result->baseline);
        break;
      case 13:
        rc = condition_case(o);
        break;
      case 14:
        rc = h2_pal_core_baseline_concurrency(runtime, &result->baseline);
        break;
      case 15:
        rc = events_case(o);
        break;
      case 16:
        rc = firmware_case(o);
        break;
      case 17:
        rc = wake_case(o, 1);
        break;
      case 18:
        rc = wake_case(o, 0);
        break;
      default:
        rc = h2_pal_core_extended_run(runtime, config, i - 19u, &o->extension);
        break;
      }
    h2_pal_result_t cleanup = release(o);
    if (cleanup == H2_PAL_OK)
      cleanup = result->baseline.cleanup_result;
    if (cleanup != H2_PAL_OK) {
      result->cleanup_result = cleanup;
      rc = cleanup;
    }
    result->cases[i].result = rc;
    result->cases[i].status =
        rc == H2_PAL_OK ? H2_PAL_CORE_E2E_PASS
        : (rc == H2_PAL_ERR_UNSUPPORTED || rc == H2_PAL_ERR_UNAVAILABLE) &&
                cleanup == H2_PAL_OK
            ? H2_PAL_CORE_E2E_BLOCKED
            : H2_PAL_CORE_E2E_FAIL;
    if (cleanup != H2_PAL_OK || result->baseline.retained_cleanup != NULL) {
      result->retained_cleanup = o;
      break;
    }
  }
  if (result->retained_cleanup == NULL) {
    h2_pal_mem_free(runtime->mem, o);
    if (config->case_begin != NULL)
      config->case_begin(config->observer_user,
                         case_ids[H2_PAL_CORE_E2E_CASE_COUNT - 1u]);
    h2_pal_core_resources_t resources_after = {0};
    if (resource_rc == H2_PAL_OK) {
      resource_rc =
          config->observe_resources(config->observer_user, &resources_after);
      if (resource_rc == H2_PAL_OK &&
          memcmp(&resources_before, &resources_after,
                 sizeof(resources_before)) != 0)
        resource_rc = H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_core_e2e_case_result_t *last =
        &result->cases[H2_PAL_CORE_E2E_CASE_COUNT - 1u];
    last->result = resource_rc;
    last->status = resource_rc == H2_PAL_OK ? H2_PAL_CORE_E2E_PASS
                   : resource_rc == H2_PAL_ERR_UNAVAILABLE ||
                           resource_rc == H2_PAL_ERR_UNSUPPORTED
                       ? H2_PAL_CORE_E2E_BLOCKED
                       : H2_PAL_CORE_E2E_FAIL;
  }
  summarize(result);
  return result->qualified      ? H2_PAL_OK
         : result->failed != 0u ? H2_PAL_ERR_INVALID_STATE
                                : H2_PAL_ERR_UNAVAILABLE;
}

const char *h2_pal_core_e2e_status_name(h2_pal_core_e2e_status_t status) {
  switch (status) {
  case H2_PAL_CORE_E2E_PASS:
    return "PASS";
  case H2_PAL_CORE_E2E_FAIL:
    return "FAIL";
  case H2_PAL_CORE_E2E_BLOCKED:
    return "BLOCKED";
  default:
    return "NOT_RUN";
  }
}
