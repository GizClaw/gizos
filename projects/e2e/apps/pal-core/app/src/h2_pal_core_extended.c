#include "h2_pal_core_extended.h"
#include "h2_pal_core_e2e_task_names.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WORKERS 8u
#define CYCLES 100u
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      return H2_PAL_ERR_INVALID_STATE;                                         \
  } while (0)
#define CALL(x)                                                                \
  do {                                                                         \
    h2_pal_result_t call_rc = (h2_pal_result_t)(x);                            \
    if (call_rc != H2_PAL_OK)                                                  \
      return call_rc;                                                          \
  } while (0)

typedef struct h2_pal_core_extended state_t;
typedef struct work {
  state_t *state;
  unsigned index, mode, calls, output;
  size_t requested_stack, observed_stack;
  h2_pal_result_t result;
} work_t;
enum {
  ONCE,
  GATED,
  RELEASER,
  SENDER,
  COUNTER,
  TRY_MUTEX,
  SEM_WAITER,
  CONDITION,
  POST_EVENT,
  UNSUBSCRIBE
};
typedef struct timer_context {
  state_t *state;
  int value;
} timer_context_t;

struct h2_pal_core_extended {
  h2_runtime_t *r;
  const h2_pal_core_e2e_config_t *cfg;
  uint32_t timeout;
  h2_pal_task_t *tasks[WORKERS];
  work_t work[WORKERS];
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *condition;
  h2_pal_semaphore_t *gate, *ready, *finished, *unsubscribe_done;
  h2_pal_queue_t *queue;
  int queue_closed, fault_active, event_initialized, self_unsubscribe;
  unsigned generation, waiting, counter, rounds;
  unsigned cleanup_gate_signals, cleanup_ready_signals;
  int stopping;
  h2_pal_system_event_subscription_t *subscription;
  h2_pal_netif_default_changed_t event_payload;
  h2_pal_system_event_t event;
  h2_pal_timer_t *timers[2];
  timer_context_t timer_contexts[2];
};

static h2_pal_result_t now(state_t *s, uint64_t *us) {
  return s->cfg->observe_monotonic_us == NULL
             ? H2_PAL_ERR_UNAVAILABLE
             : s->cfg->observe_monotonic_us(s->cfg->observer_user, us);
}
static h2_pal_result_t resources(state_t *s, h2_pal_core_resources_t *out) {
  memset(out, 0, sizeof(*out));
  return s->cfg->observe_resources == NULL
             ? H2_PAL_ERR_UNAVAILABLE
             : s->cfg->observe_resources(s->cfg->observer_user, out);
}
static h2_pal_result_t balanced(state_t *s,
                                const h2_pal_core_resources_t *before) {
  h2_pal_core_resources_t after;
  CALL(resources(s, &after));
  CHECK(memcmp(before, &after, sizeof(after)) == 0);
  return H2_PAL_OK;
}
static h2_pal_result_t setup(state_t *s) {
  const h2_pal_mutex_config_t m = {.name = "pal-core-contract",
                                   .allocator = s->r->mem};
  CALL(h2_pal_mutex_create(s->r->sync, &m, &s->mutex));
  const h2_pal_semaphore_config_t sem = {
      .name = "pal-core-contract", .allocator = s->r->mem, .max_count = 4096u};
  CALL(h2_pal_semaphore_create(s->r->sync, &sem, &s->ready));
  CALL(h2_pal_semaphore_create(s->r->sync, &sem, &s->gate));
  return h2_pal_semaphore_create(s->r->sync, &sem, &s->finished);
}
static h2_pal_result_t queue_create(state_t *s, size_t count) {
  const h2_pal_queue_config_t q = {.name = "pal-core-contract",
                                   .item_size = sizeof(int),
                                   .item_count = count,
                                   .allocator = s->r->mem};
  return (h2_pal_result_t)h2_pal_queue_create(s->r->queue, &q, &s->queue);
}
static h2_pal_result_t wait_ready(state_t *s, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    CALL(h2_pal_semaphore_take(s->r->sync, s->ready, s->timeout));
  return H2_PAL_OK;
}
static h2_pal_result_t release_gate(state_t *s, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    CALL(h2_pal_semaphore_give(s->r->sync, s->gate));
  return H2_PAL_OK;
}
static h2_pal_result_t join_one(state_t *s, unsigned i) {
  if (s->tasks[i] == NULL)
    return H2_PAL_OK;
  uint64_t begin, current;
  CALL(now(s, &begin));
  for (uint32_t attempt = 0; attempt <= s->timeout; ++attempt) {
    h2_pal_result_t rc = h2_pal_task_join(s->r->task, s->tasks[i]);
    if (rc == H2_PAL_OK) {
      s->tasks[i] = NULL;
      return rc;
    }
    if (rc != H2_PAL_ERR_BUSY)
      return rc;
    CALL(now(s, &current));
    if (current - begin >= (uint64_t)s->timeout * 1000u)
      return H2_PAL_ERR_TIMEOUT;
    CALL(h2_pal_time_sleep_ms(s->r->time, 1u));
  }
  return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t do_work(work_t *w) {
  state_t *s = w->state;
  if (w->mode == TRY_MUTEX) {
    ++w->calls;
    h2_pal_result_t rc = h2_pal_mutex_try_lock(s->r->sync, s->mutex);
    if (rc == H2_PAL_OK) {
      (void)h2_pal_mutex_unlock(s->r->sync, s->mutex);
      return H2_PAL_ERR_INVALID_STATE;
    }
    return rc == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_OK : rc;
  }
  CALL(h2_pal_mutex_lock(s->r->sync, s->mutex));
  ++w->calls;
  CALL(h2_pal_mutex_unlock(s->r->sync, s->mutex));
  if (w->requested_stack != 0u) {
    if (s->cfg->observe_task_stack == NULL)
      return H2_PAL_ERR_UNAVAILABLE;
    CALL(s->cfg->observe_task_stack(s->cfg->observer_user, &w->observed_stack));
    CHECK(w->observed_stack >= w->requested_stack);
  }
  switch (w->mode) {
  case GATED:
  case COUNTER:
  case SEM_WAITER:
    CALL(h2_pal_semaphore_give(s->r->sync, s->ready));
    CALL(h2_pal_semaphore_take(s->r->sync, s->gate, s->timeout));
    if (w->mode == COUNTER)
      for (unsigned n = 0; n < 1000u; ++n) {
        CALL(h2_pal_mutex_lock(s->r->sync, s->mutex));
        ++s->counter;
        CALL(h2_pal_mutex_unlock(s->r->sync, s->mutex));
      }
    break;
  case RELEASER:
    CALL(wait_ready(s, 1u));
    CALL(h2_pal_time_sleep_ms(s->r->time, 20u));
    CALL(release_gate(s, 1u));
    break;
  case SENDER: {
    int value = 113;
    CALL(h2_pal_semaphore_give(s->r->sync, s->ready));
    return (h2_pal_result_t)h2_pal_queue_send(s->r->queue, s->queue, &value,
                                              s->timeout);
  }
  case CONDITION: {
    CALL(h2_pal_mutex_lock(s->r->sync, s->mutex));
    h2_pal_result_t rc = H2_PAL_OK;
    for (unsigned round = 1;
         round <= s->rounds && !s->stopping && rc == H2_PAL_OK; ++round) {
      ++s->waiting;
      while (s->generation < round && !s->stopping && rc == H2_PAL_OK) {
        rc = h2_pal_cond_wait(s->r->sync, s->condition, s->mutex, s->timeout);
      }
      if (rc == H2_PAL_OK && !s->stopping)
        ++s->counter;
    }
    h2_pal_result_t unlock = h2_pal_mutex_unlock(s->r->sync, s->mutex);
    return rc == H2_PAL_OK ? unlock : rc;
  }
  case POST_EVENT:
    return (h2_pal_result_t)h2_pal_system_event_post(s->cfg->event_fixture,
                                                     &s->event, s->timeout);
  case UNSUBSCRIBE:
    CALL(h2_pal_semaphore_give(s->r->sync, s->ready));
    h2_pal_system_event_unsubscribe(s->cfg->event_fixture, s->subscription);
    s->subscription = NULL;
    break;
  default:
    break;
  }
  w->output = (w->index + 1u) * 7919u;
  return H2_PAL_OK;
}
static void entry(void *user) {
  work_t *w = user;
  w->result = do_work(w);
  if (w->mode == UNSUBSCRIBE && w->state->unsubscribe_done != NULL)
    (void)h2_pal_semaphore_give(w->state->r->sync, w->state->unsubscribe_done);
  (void)h2_pal_semaphore_give(w->state->r->sync, w->state->finished);
}
static h2_pal_result_t start(state_t *s, unsigned i, unsigned mode,
                             size_t stack) {
  s->work[i] = (work_t){.state = s,
                        .index = i,
                        .mode = mode,
                        .requested_stack = stack,
                        .result = H2_PAL_ERR_INVALID_STATE};
  const h2_pal_task_options_t opt = {.name = h2_pal_core_e2e_core_task_name,
                                     .min_stack_size = stack};
  return h2_pal_task_start(s->r->task, &opt, entry, &s->work[i], &s->tasks[i]);
}
static h2_pal_result_t check_workers(state_t *s, unsigned count, int reverse) {
  for (unsigned n = 0; n < count; ++n) {
    unsigned i = reverse ? count - n - 1u : n;
    CALL(join_one(s, i));
    CHECK(s->work[i].calls == 1u && s->work[i].result == H2_PAL_OK);
    CHECK(s->work[i].output == (i + 1u) * 7919u);
  }
  return H2_PAL_OK;
}
static void noop(void *user) { (void)user; }

static h2_pal_result_t arguments(state_t *s) {
  h2_runtime_t *r = s->r;
  h2_pal_task_t *task = NULL;
  h2_pal_timer_t *timer = NULL;
  h2_pal_queue_t *queue = NULL;
  h2_pal_mutex_t *mutex = NULL;
  h2_pal_semaphore_t *sem = NULL;
  h2_pal_cond_t *cond = NULL;
  h2_pal_system_event_subscription_t *sub = NULL;
  CHECK(h2_pal_mem_alloc(NULL, 8) == NULL &&
        h2_pal_mem_realloc(NULL, NULL, 8) == NULL);
  h2_pal_mem_free(NULL, NULL);
  CHECK(h2_pal_log_write(r->log, H2_PAL_LOG_INFO, NULL, NULL) ==
        H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_time_get_monotonic_ms(r->time, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_time_get_monotonic_us(r->time, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_time_get_wall_ms(r->time, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_time_get_wall_status(r->time, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_time_set_wall_ms(NULL, 0) == H2_PAL_ERR_UNSUPPORTED);
  CHECK(h2_pal_time_sleep_ms(NULL, 0) == H2_PAL_ERR_UNSUPPORTED);
  CHECK(h2_pal_task_start(r->task, NULL, NULL, NULL, &task) ==
        H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_task_start(r->task, NULL, noop, NULL, NULL) ==
        H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_task_join(r->task, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_timer_create(r->timer, NULL, &timer) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_timer_destroy(r->timer, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_timer_start(r->timer, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_timer_stop(r->timer, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_timer_reset(r->timer, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_timer_set_period_ms(r->timer, NULL, 0) ==
        H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_timer_is_running(r->timer, NULL, NULL) ==
        H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_queue_create(r->queue, NULL, &queue) == H2_PAL_ERR_INVALID_ARG);
  h2_pal_queue_destroy(r->queue, NULL);
  CHECK(h2_pal_queue_send(r->queue, NULL, NULL, 0) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_queue_send_latest(r->queue, NULL, NULL) ==
        H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_queue_recv(r->queue, NULL, NULL, 0) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_queue_reset(r->queue, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_queue_close(r->queue, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_mutex_create(r->sync, NULL, &mutex) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_mutex_destroy(r->sync, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_mutex_lock(r->sync, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_mutex_try_lock(r->sync, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_mutex_unlock(r->sync, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_semaphore_create(r->sync, NULL, &sem) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_semaphore_destroy(r->sync, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_semaphore_take(r->sync, NULL, 0) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_semaphore_give(r->sync, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_cond_create(r->sync, NULL, &cond) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_cond_destroy(r->sync, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_cond_wait(r->sync, NULL, NULL, 0) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_cond_signal(r->sync, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_cond_broadcast(r->sync, NULL) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_system_event_init(NULL) == H2_PAL_ERR_INVALID_ARG);
  h2_pal_system_event_deinit(NULL);
  const h2_pal_system_event_t invalid = {0};
  CHECK(h2_pal_system_event_validate(&invalid) == H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_system_event_post(s->cfg->event_fixture, &invalid, 0) ==
        H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_system_event_subscribe(s->cfg->event_fixture,
                                      H2_PAL_SYSTEM_EVENT_TYPE_NONE, NULL, NULL,
                                      &sub) == H2_PAL_ERR_INVALID_ARG);
  h2_pal_system_event_unsubscribe(NULL, NULL);
  CHECK(h2_pal_firmware_info_get_current(r->firmware_info, NULL) ==
        H2_PAL_ERR_INVALID_ARG);
  return H2_PAL_OK;
}
static size_t required_memory_alignment(void) {
#if defined(_MSC_VER) && !defined(__clang__)
  /* MSVC C11 supports _Alignof but does not declare max_align_t in stddef.h.
   * The union covers every fundamental type the MSVC C allocator promises. */
  typedef union h2_pal_core_max_align {
    long double floating;
    long long integer;
    void *pointer;
  } h2_pal_core_max_align_t;
  return _Alignof(h2_pal_core_max_align_t);
#else
  return _Alignof(max_align_t);
#endif
}

static h2_pal_result_t memory_churn(state_t *s) {
  h2_pal_core_resources_t before;
  CALL(resources(s, &before));
  for (unsigned n = 0; n < CYCLES; ++n) {
    size_t len = 1u + n * 17u;
    unsigned char *p = h2_pal_mem_alloc(s->r->mem, len);
    if (p == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    int valid = (uintptr_t)p % required_memory_alignment() == 0;
    memset(p, (int)(n & 255u), len);
    unsigned char *next = h2_pal_mem_realloc(s->r->mem, p, len + 31u);
    if (next == NULL) {
      h2_pal_mem_free(s->r->mem, p);
      return H2_PAL_ERR_NO_MEMORY;
    }
    valid = valid && (uintptr_t)next % required_memory_alignment() == 0;
    for (size_t k = 0; k < len; ++k)
      if (next[k] != (unsigned char)n)
        valid = 0;
    h2_pal_mem_free(s->r->mem, next);
    CHECK(valid);
  }
  return balanced(s, &before);
}
static h2_pal_result_t task_case(state_t *s, unsigned which) {
  if (which == 7u && s->cfg->observe_task_stack == NULL)
    return H2_PAL_ERR_UNAVAILABLE;
  if ((which == 8u || which == 9u) && s->cfg->task_allocation_fault == NULL)
    return H2_PAL_ERR_UNAVAILABLE;
  CALL(setup(s));
  h2_pal_core_resources_t before = {0};
  const int measure_resources =
      which >= 5u || s->cfg->observe_resources != NULL;
  if (measure_resources)
    CALL(resources(s, &before));
  if (which == 2u) {
    for (unsigned i = 0; i < WORKERS; ++i)
      CALL(start(s, i, GATED, 0));
    CALL(wait_ready(s, WORKERS));
    CALL(release_gate(s, WORKERS));
    CALL(check_workers(s, WORKERS, 1));
  } else if (which == 3u) {
    CALL(start(s, 0, GATED, 0));
    CALL(start(s, 1, RELEASER, 0));
    CALL(check_workers(s, 2u, 0));
  } else if (which == 4u) {
    CALL(start(s, 0, ONCE, 0));
    CALL(h2_pal_semaphore_take(s->r->sync, s->finished, s->timeout));
    CALL(check_workers(s, 1u, 0));
  } else if (which == 5u || which == 6u) {
    unsigned batch = which == 5u ? 1u : WORKERS;
    unsigned cycles = which == 5u ? CYCLES : 16u;
    for (unsigned n = 0; n < cycles; ++n) {
      for (unsigned i = 0; i < batch; ++i)
        CALL(start(s, i, ONCE, 0));
      CALL(check_workers(s, batch, n % 2u));
      CALL(balanced(s, &before));
    }
  } else if (which == 7u) {
    const size_t sizes[] = {4096u, 16384u, 65536u};
    for (unsigned i = 0; i < 3; ++i) {
      CALL(start(s, 0, ONCE, sizes[i]));
      h2_pal_core_resources_t live;
      CALL(resources(s, &live));
      CHECK(live.tasks == before.tasks + 1u &&
            live.task_stack_bytes >= before.task_stack_bytes + sizes[i]);
      CALL(check_workers(s, 1, 0));
      CALL(balanced(s, &before));
    }
  } else {
    unsigned success = which == 8u ? 0u : 2u;
    CALL(s->cfg->task_allocation_fault(s->cfg->observer_user, (int)success));
    s->fault_active = 1;
    for (unsigned i = 0; i < success; ++i)
      CALL(start(s, i, GATED, 4096u));
    h2_pal_result_t rc = start(s, success, ONCE, 4096u);
    CHECK(rc == H2_PAL_ERR_NO_MEMORY && s->tasks[success] == NULL &&
          s->work[success].calls == 0u);
    CALL(s->cfg->task_allocation_fault(s->cfg->observer_user, -1));
    s->fault_active = 0;
    CALL(wait_ready(s, success));
    CALL(release_gate(s, success));
    CALL(check_workers(s, success, 1));
    CALL(balanced(s, &before));
    CALL(start(s, 0, ONCE, 4096u));
    CALL(check_workers(s, 1u, 0));
  }
  return measure_resources ? balanced(s, &before) : H2_PAL_OK;
}
static h2_pal_result_t queue_case(state_t *s, unsigned which) {
  CALL(setup(s));
  CALL(queue_create(s, 1));
  int value = 71, received = 0;
  CALL(h2_pal_queue_send(s->r->queue, s->queue, &value, 0));
  if (which == 10u) {
    value = -1;
    CALL(h2_pal_queue_close(s->r->queue, s->queue));
    s->queue_closed = 1;
    CALL(h2_pal_queue_recv(s->r->queue, s->queue, &received, 0));
    CHECK(received == 71);
    CHECK(h2_pal_queue_recv(s->r->queue, s->queue, &received, 0) ==
          H2_PAL_ERR_CLOSED);
  } else {
    CALL(start(s, 0, SENDER, 0));
    CALL(wait_ready(s, 1));
    CHECK(h2_pal_semaphore_take(s->r->sync, s->finished, 10u) ==
          H2_PAL_ERR_TIMEOUT);
    if (which == 11u) {
      CALL(h2_pal_queue_close(s->r->queue, s->queue));
      s->queue_closed = 1;
    } else
      CALL(h2_pal_queue_reset(s->r->queue, s->queue));
    CALL(join_one(s, 0));
    CHECK(s->work[0].calls == 1u &&
          s->work[0].result == (which == 11u ? H2_PAL_ERR_CLOSED : H2_PAL_OK));
    if (which == 12u) {
      CALL(h2_pal_queue_recv(s->r->queue, s->queue, &received, 0));
      CHECK(received == 113);
    }
  }
  return H2_PAL_OK;
}
static h2_pal_result_t mutex_contention(state_t *s) {
  CALL(setup(s));
  CALL(h2_pal_mutex_lock(s->r->sync, s->mutex));
  h2_pal_result_t probe = start(s, 0, TRY_MUTEX, 0);
  if (probe == H2_PAL_OK)
    probe = join_one(s, 0);
  h2_pal_result_t unlock = h2_pal_mutex_unlock(s->r->sync, s->mutex);
  if (probe != H2_PAL_OK)
    return probe;
  if (unlock != H2_PAL_OK)
    return unlock;
  CHECK(s->work[0].calls == 1u && s->work[0].result == H2_PAL_OK);
  for (unsigned i = 0; i < WORKERS; ++i)
    CALL(start(s, i, COUNTER, 0));
  CALL(wait_ready(s, WORKERS));
  CALL(release_gate(s, WORKERS));
  CALL(check_workers(s, WORKERS, 1));
  CHECK(s->counter == WORKERS * 1000u);
  return H2_PAL_OK;
}
static h2_pal_result_t semaphore_waiters(state_t *s) {
  CALL(setup(s));
  for (unsigned i = 0; i < WORKERS; ++i)
    CALL(start(s, i, SEM_WAITER, 0));
  CALL(wait_ready(s, WORKERS));
  CHECK(h2_pal_semaphore_take(s->r->sync, s->finished, 10u) ==
        H2_PAL_ERR_TIMEOUT);
  CALL(release_gate(s, WORKERS));
  CALL(check_workers(s, WORKERS, 1));
  CHECK(h2_pal_semaphore_take(s->r->sync, s->gate, 0) == H2_PAL_ERR_TIMEOUT);
  return H2_PAL_OK;
}
static h2_pal_result_t condition_rounds(state_t *s, unsigned rounds) {
  CALL(setup(s));
  const h2_pal_cond_config_t cfg = {.name = "pal-core-generations",
                                    .allocator = s->r->mem};
  CALL(h2_pal_cond_create(s->r->sync, &cfg, &s->condition));
  s->rounds = rounds;
  for (unsigned i = 0; i < WORKERS; ++i)
    CALL(start(s, i, CONDITION, 0));
  for (unsigned generation = 1; generation <= rounds; ++generation) {
    uint64_t begin, current;
    CALL(now(s, &begin));
    for (;;) {
      CALL(h2_pal_mutex_lock(s->r->sync, s->mutex));
      if (s->waiting == generation * WORKERS) {
        s->generation = generation;
        h2_pal_result_t rc = h2_pal_cond_broadcast(s->r->sync, s->condition);
        h2_pal_result_t unlock = h2_pal_mutex_unlock(s->r->sync, s->mutex);
        if (rc != H2_PAL_OK)
          return rc;
        if (unlock != H2_PAL_OK)
          return unlock;
        break;
      }
      CALL(h2_pal_mutex_unlock(s->r->sync, s->mutex));
      CALL(now(s, &current));
      if (current - begin >= (uint64_t)s->timeout * 1000u)
        return H2_PAL_ERR_TIMEOUT;
      CALL(h2_pal_time_sleep_ms(s->r->time, 1u));
    }
  }
  for (unsigned i = 0; i < WORKERS; ++i) {
    CALL(join_one(s, i));
    CHECK(s->work[i].calls == 1u && s->work[i].result == H2_PAL_OK);
  }
  CHECK(s->counter == rounds * WORKERS);
  return H2_PAL_OK;
}
static int event_handler(void *user, const h2_pal_system_event_t *event) {
  state_t *s = user;
  if (event->source_id != 0x50414c2u ||
      event->payload_size != sizeof(s->event_payload))
    return H2_PAL_ERR_INVALID_STATE;
  if (s->self_unsubscribe) {
    h2_pal_system_event_subscription_t *sub = s->subscription;
    s->subscription = NULL;
    h2_pal_system_event_unsubscribe(s->cfg->event_fixture, sub);
    return h2_pal_semaphore_give(s->r->sync, s->ready);
  }
  h2_pal_result_t rc = h2_pal_semaphore_give(s->r->sync, s->ready);
  return rc == H2_PAL_OK
             ? h2_pal_semaphore_take(s->r->sync, s->gate, s->timeout)
             : rc;
}
static h2_pal_result_t event_case(state_t *s, int self) {
  if (s->cfg->event_fixture == NULL)
    return H2_PAL_ERR_UNAVAILABLE;
  CALL(setup(s));
  CALL(h2_pal_system_event_init(s->cfg->event_fixture));
  s->event_initialized = 1;
  s->self_unsubscribe = self;
  s->event = (h2_pal_system_event_t){
      .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
      .source_id = 0x50414c2u,
      .timestamp_ms = 123u,
      .payload = &s->event_payload,
      .payload_size = sizeof(s->event_payload)};
  CALL(h2_pal_system_event_subscribe(s->cfg->event_fixture, s->event.type,
                                     event_handler, s, &s->subscription));
  CALL(start(s, 0, POST_EVENT, 0));
  CALL(wait_ready(s, 1));
  if (self) {
    CALL(join_one(s, 0));
    CHECK(s->work[0].result == H2_PAL_OK);
    CALL(
        h2_pal_system_event_post(s->cfg->event_fixture, &s->event, s->timeout));
    CHECK(h2_pal_semaphore_take(s->r->sync, s->ready, 20u) ==
          H2_PAL_ERR_TIMEOUT);
  } else {
    const h2_pal_semaphore_config_t done = {
        .name = "pal-core-unsubscribe", .allocator = s->r->mem, .max_count = 1u};
    CALL(h2_pal_semaphore_create(s->r->sync, &done, &s->unsubscribe_done));
    CALL(start(s, 1, UNSUBSCRIBE, 0));
    CALL(wait_ready(s, 1));
    /* post may complete before an asynchronous event handler does. Only the
     * unsubscribe worker can satisfy this quiescence check. */
    CHECK(h2_pal_semaphore_take(s->r->sync, s->unsubscribe_done, 20u) ==
          H2_PAL_ERR_TIMEOUT);
    CALL(release_gate(s, 1));
    CALL(join_one(s, 0));
    CALL(join_one(s, 1));
    CHECK(s->work[0].result == H2_PAL_OK && s->work[1].result == H2_PAL_OK &&
          s->subscription == NULL);
    CALL(
        h2_pal_system_event_post(s->cfg->event_fixture, &s->event, s->timeout));
    CHECK(h2_pal_semaphore_take(s->r->sync, s->ready, 20u) ==
          H2_PAL_ERR_TIMEOUT);
  }
  return H2_PAL_OK;
}
static void timer_callback(void *user, h2_pal_timer_t *timer) {
  (void)timer;
  timer_context_t *ctx = user;
  (void)h2_pal_queue_send(ctx->state->r->queue, ctx->state->queue, &ctx->value,
                          0);
}
static h2_pal_result_t timer_create(state_t *s, unsigned slot, int value,
                                    unsigned flags) {
  s->timer_contexts[slot] = (timer_context_t){.state = s, .value = value};
  const h2_pal_timer_config_t cfg = {.name = "pal-core-timer-contract",
                                     .period_ms = 3u,
                                     .flags = flags,
                                     .cb = timer_callback,
                                     .cb_user = &s->timer_contexts[slot]};
  return h2_pal_timer_create(s->r->timer, &cfg, &s->timers[slot]);
}
static h2_pal_result_t timer_case(state_t *s, int churn) {
  CALL(queue_create(s, 64u));
  h2_pal_core_resources_t before = {0};
  if (churn || s->cfg->observe_resources != NULL)
    CALL(resources(s, &before));
  int value = 0;
  if (churn) {
    for (unsigned n = 0; n < CYCLES; ++n) {
      CALL(timer_create(s, 0, (int)n, H2_PAL_TIMER_FLAG_AUTO_START));
      CALL(h2_pal_queue_recv(s->r->queue, s->queue, &value, s->timeout));
      CHECK(value == (int)n);
      CALL(h2_pal_timer_destroy(s->r->timer, s->timers[0]));
      s->timers[0] = NULL;
      CHECK(h2_pal_queue_recv(s->r->queue, s->queue, &value, 0) ==
            H2_PAL_ERR_TIMEOUT);
      CALL(balanced(s, &before));
    }
  } else {
    CALL(timer_create(s, 0, 1,
                      H2_PAL_TIMER_FLAG_REPEAT | H2_PAL_TIMER_FLAG_AUTO_START));
    CALL(timer_create(s, 1, 2,
                      H2_PAL_TIMER_FLAG_REPEAT | H2_PAL_TIMER_FLAG_AUTO_START));
    unsigned counts[2] = {0};
    for (unsigned n = 0; n < 64u && (counts[0] < 2u || counts[1] < 2u); ++n) {
      CALL(h2_pal_queue_recv(s->r->queue, s->queue, &value, s->timeout));
      CHECK(value == 1 || value == 2);
      ++counts[value - 1];
    }
    CHECK(counts[0] >= 2u && counts[1] >= 2u);
    CALL(h2_pal_timer_stop(s->r->timer, s->timers[0]));
    CALL(h2_pal_queue_reset(s->r->queue, s->queue));
    for (unsigned n = 0; n < 3u; ++n) {
      CALL(h2_pal_queue_recv(s->r->queue, s->queue, &value, s->timeout));
      CHECK(value == 2);
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_pal_core_extended_cleanup(state_t **owner) {
  state_t *s = *owner;
  if (s == NULL)
    return H2_PAL_OK;
  if (s->fault_active) {
    CALL(s->cfg->task_allocation_fault(s->cfg->observer_user, -1));
    s->fault_active = 0;
  }
  if (s->mutex != NULL && s->condition != NULL) {
    CALL(h2_pal_mutex_lock(s->r->sync, s->mutex));
    s->stopping = 1;
    h2_pal_result_t rc = h2_pal_cond_broadcast(s->r->sync, s->condition);
    h2_pal_result_t unlock = h2_pal_mutex_unlock(s->r->sync, s->mutex);
    if (rc != H2_PAL_OK)
      return rc;
    if (unlock != H2_PAL_OK)
      return unlock;
  }
  if (s->gate != NULL)
    while (s->cleanup_gate_signals < WORKERS) {
      CALL(h2_pal_semaphore_give(s->r->sync, s->gate));
      ++s->cleanup_gate_signals;
    }
  if (s->ready != NULL)
    while (s->cleanup_ready_signals < WORKERS) {
      CALL(h2_pal_semaphore_give(s->r->sync, s->ready));
      ++s->cleanup_ready_signals;
    }
  if (s->queue != NULL && !s->queue_closed) {
    CALL(h2_pal_queue_close(s->r->queue, s->queue));
    s->queue_closed = 1;
  }
  for (unsigned i = 0; i < WORKERS; ++i)
    CALL(join_one(s, i));
  for (unsigned i = 0; i < 2u; ++i)
    if (s->timers[i] != NULL) {
      CALL(h2_pal_timer_destroy(s->r->timer, s->timers[i]));
      s->timers[i] = NULL;
    }
  if (s->event_initialized) {
    h2_pal_system_event_unsubscribe(s->cfg->event_fixture, s->subscription);
    s->subscription = NULL;
    h2_pal_system_event_deinit(s->cfg->event_fixture);
    s->event_initialized = 0;
  }
  if (s->queue != NULL) {
    h2_pal_queue_destroy(s->r->queue, s->queue);
    s->queue = NULL;
  }
  if (s->condition != NULL) {
    CALL(h2_pal_cond_destroy(s->r->sync, s->condition));
    s->condition = NULL;
  }
  if (s->mutex != NULL) {
    CALL(h2_pal_mutex_destroy(s->r->sync, s->mutex));
    s->mutex = NULL;
  }
  if (s->gate != NULL) {
    CALL(h2_pal_semaphore_destroy(s->r->sync, s->gate));
    s->gate = NULL;
  }
  if (s->ready != NULL) {
    CALL(h2_pal_semaphore_destroy(s->r->sync, s->ready));
    s->ready = NULL;
  }
  if (s->unsubscribe_done != NULL) {
    CALL(h2_pal_semaphore_destroy(s->r->sync, s->unsubscribe_done));
    s->unsubscribe_done = NULL;
  }
  if (s->finished != NULL) {
    CALL(h2_pal_semaphore_destroy(s->r->sync, s->finished));
    s->finished = NULL;
  }
  h2_pal_mem_free(s->r->mem, s);
  *owner = NULL;
  return H2_PAL_OK;
}
h2_pal_result_t h2_pal_core_extended_run(h2_runtime_t *r,
                                         const h2_pal_core_e2e_config_t *cfg,
                                         size_t index, state_t **owner) {
  if (cfg->observe_monotonic_us == NULL)
    return H2_PAL_ERR_UNAVAILABLE;
  state_t *s = h2_pal_mem_alloc(r->mem, sizeof(*s));
  if (s == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(s, 0, sizeof(*s));
  s->r = r;
  s->cfg = cfg;
  s->timeout = cfg->timeout_ms ? cfg->timeout_ms : 2000u;
  *owner = s;
  if (index == 0u)
    return cfg->event_fixture == NULL ? H2_PAL_ERR_UNAVAILABLE : arguments(s);
  if (index == 1u)
    return memory_churn(s);
  if (index <= 9u)
    return task_case(s, (unsigned)index);
  if (index <= 12u)
    return queue_case(s, (unsigned)index);
  if (index == 13u)
    return mutex_contention(s);
  if (index == 14u)
    return semaphore_waiters(s);
  if (index == 15u || index == 16u)
    return condition_rounds(s, index == 15u ? 1u : 20u);
  if (index == 17u || index == 18u)
    return event_case(s, index == 18u);
  if (index == 19u || index == 20u)
    return timer_case(s, index == 20u);
  return H2_PAL_ERR_INVALID_ARG;
}
