/* Host test for the BK7258 AP condition variable.
 *
 * The sync translation unit is included as-is; the SDK mutex and semaphore
 * wrappers it calls are backed by pthreads here, so the condition's own
 * bookkeeping (per-waiter binary semaphore, waiter list, timeout unlink) runs
 * on the host exactly as it does on the AP core. The scenario that broke the
 * previous shared-token design is exercised directly: a waiter blocked without
 * a timeout must be woken by a broadcast even while a second waiter keeps
 * re-entering the wait with a 1 ms timeout. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "h2_bk_platform_sync.c"

/* ---- pthread-backed stand-ins for the SDK primitives ------------------- */

typedef struct host_sem {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  unsigned count;
  unsigned max;
  bool recursive;
  pthread_t owner;
  unsigned depth;
} host_sem_t;

_Static_assert(sizeof(host_sem_t) <= sizeof(((StaticSemaphore_t *)0)->opaque),
               "StaticSemaphore_t shim too small for host_sem_t");

static host_sem_t *host_sem_init(StaticSemaphore_t *storage, unsigned max,
                                 unsigned initial, bool recursive) {
  host_sem_t *sem = (host_sem_t *)storage->opaque;
  memset(sem, 0, sizeof(*sem));
  assert(pthread_mutex_init(&sem->lock, NULL) == 0);
  assert(pthread_cond_init(&sem->cond, NULL) == 0);
  sem->count = initial;
  sem->max = max;
  sem->recursive = recursive;
  return sem;
}

static int host_sem_take(host_sem_t *sem, uint32_t timeout_ms) {
  struct timespec deadline;
  if (timeout_ms != BEKEN_WAIT_FOREVER) {
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += (time_t)(timeout_ms / 1000u);
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec += 1;
      deadline.tv_nsec -= 1000000000L;
    }
  }
  assert(pthread_mutex_lock(&sem->lock) == 0);
  if (sem->recursive && sem->depth != 0u &&
      pthread_equal(sem->owner, pthread_self())) {
    sem->depth++;
    assert(pthread_mutex_unlock(&sem->lock) == 0);
    return kNoErr;
  }
  int rc = 0;
  while (sem->count == 0u && rc == 0) {
    if (timeout_ms == BEKEN_WAIT_FOREVER) {
      rc = pthread_cond_wait(&sem->cond, &sem->lock);
    } else if (timeout_ms == 0u) {
      rc = ETIMEDOUT;
    } else {
      rc = pthread_cond_timedwait(&sem->cond, &sem->lock, &deadline);
    }
  }
  int result = kGeneralErr;
  if (sem->count != 0u) {
    sem->count--;
    if (sem->recursive) {
      sem->owner = pthread_self();
      sem->depth = 1u;
    }
    result = kNoErr;
  }
  assert(pthread_mutex_unlock(&sem->lock) == 0);
  return result;
}

static int host_sem_give(host_sem_t *sem) {
  assert(pthread_mutex_lock(&sem->lock) == 0);
  int result = kGeneralErr;
  if (sem->recursive && sem->depth > 1u) {
    sem->depth--;
    result = kNoErr;
  } else if (sem->count < sem->max) {
    sem->count++;
    sem->depth = 0u;
    result = kNoErr;
    assert(pthread_cond_signal(&sem->cond) == 0);
  }
  assert(pthread_mutex_unlock(&sem->lock) == 0);
  return result;
}

static void host_sem_deinit(host_sem_t *sem) {
  assert(pthread_cond_destroy(&sem->cond) == 0);
  assert(pthread_mutex_destroy(&sem->lock) == 0);
}

static unsigned binary_semaphores_created;

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage) {
  binary_semaphores_created++;
  return host_sem_init(storage, 1u, 0u, false);
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
  return host_sem_init(storage, 1u, 1u, false);
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(
    StaticSemaphore_t *storage) {
  return host_sem_init(storage, 1u, 1u, true);
}
SemaphoreHandle_t xSemaphoreCreateCountingStatic(UBaseType_t max_count,
                                                 UBaseType_t initial_count,
                                                 StaticSemaphore_t *storage) {
  return host_sem_init(storage, max_count, initial_count, false);
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout) {
  return host_sem_take(semaphore, timeout) == kNoErr ? pdPASS : 0;
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore,
                                   uint32_t timeout) {
  return host_sem_take(semaphore, timeout) == kNoErr ? pdPASS : 0;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  return host_sem_give(semaphore) == kNoErr ? pdPASS : 0;
}
int rtos_lock_mutex(beken_mutex_t *mutex) {
  return host_sem_take(*mutex, BEKEN_WAIT_FOREVER);
}
int rtos_trylock_mutex(beken_mutex_t *mutex) { return host_sem_take(*mutex, 0u); }
int rtos_unlock_mutex(beken_mutex_t *mutex) { return host_sem_give(*mutex); }
int rtos_deinit_mutex(beken_mutex_t *mutex) {
  host_sem_deinit(*mutex);
  return kNoErr;
}
int rtos_lock_recursive_mutex(beken_mutex_t *mutex) {
  return host_sem_take(*mutex, BEKEN_WAIT_FOREVER);
}
int rtos_unlock_recursive_mutex(beken_mutex_t *mutex) {
  return host_sem_give(*mutex);
}
int rtos_deinit_recursive_mutex(beken_mutex_t *mutex) {
  host_sem_deinit(*mutex);
  return kNoErr;
}
int rtos_get_semaphore(beken_semaphore_t *semaphore, uint32_t timeout_ms) {
  return host_sem_take(*semaphore, timeout_ms);
}
int rtos_set_semaphore(beken_semaphore_t *semaphore) {
  return host_sem_give(*semaphore);
}
int rtos_deinit_semaphore(beken_semaphore_t *semaphore) {
  host_sem_deinit(*semaphore);
  return kNoErr;
}
void *os_memset(void *ptr, int value, size_t size) {
  return memset(ptr, value, size);
}
void *os_malloc(size_t size) { return malloc(size); }
void os_free(void *ptr) { free(ptr); }

/* ---- test harness -------------------------------------------------------- */

static void sleep_ms(unsigned ms) {
  const struct timespec delay = {.tv_sec = ms / 1000u,
                                 .tv_nsec = (long)(ms % 1000u) * 1000000L};
  (void)nanosleep(&delay, NULL);
}

typedef struct fixture {
  const h2_pal_sync_api_t *api;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *cond;
  volatile bool done;
  volatile bool stop;
} fixture_t;

typedef struct waiter {
  fixture_t *fx;
  uint32_t timeout_ms;
  pthread_t thread;
  volatile bool waiting;
  volatile bool returned;
  h2_pal_result_t result;
  unsigned wakeups;
} waiter_t;

/* Waits until fx->done is set, the way a real consumer loops on its predicate. */
static void *forever_waiter(void *user) {
  waiter_t *w = user;
  fixture_t *fx = w->fx;
  assert(h2_pal_mutex_lock(fx->api, fx->mutex) == H2_PAL_OK);
  while (!fx->done) {
    /* Set while holding the mutex: once main sees it and takes the mutex,
     * this thread is registered on the condition (wait registers before it
     * releases the mutex). */
    w->waiting = true;
    w->result = h2_pal_cond_wait(fx->api, fx->cond, fx->mutex, w->timeout_ms);
    assert(w->result == H2_PAL_OK);
    w->wakeups++;
  }
  assert(h2_pal_mutex_try_lock(fx->api, fx->mutex) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_pal_mutex_unlock(fx->api, fx->mutex) == H2_PAL_OK);
  w->returned = true;
  return NULL;
}

/* Re-enters the wait with a short timeout until told to stop, like the
 * GizClaw downlink worker polling progress_cond every 20 ms. */
static void *polling_waiter(void *user) {
  waiter_t *w = user;
  fixture_t *fx = w->fx;
  assert(h2_pal_mutex_lock(fx->api, fx->mutex) == H2_PAL_OK);
  while (!fx->stop) {
    h2_pal_result_t rc =
        h2_pal_cond_wait(fx->api, fx->cond, fx->mutex, w->timeout_ms);
    assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_TIMEOUT);
    if (rc == H2_PAL_OK) {
      w->wakeups++;
    }
  }
  assert(h2_pal_mutex_unlock(fx->api, fx->mutex) == H2_PAL_OK);
  w->returned = true;
  return NULL;
}

/* Single wait; records the result. */
static void *single_waiter(void *user) {
  waiter_t *w = user;
  fixture_t *fx = w->fx;
  assert(h2_pal_mutex_lock(fx->api, fx->mutex) == H2_PAL_OK);
  w->result = h2_pal_cond_wait(fx->api, fx->cond, fx->mutex, w->timeout_ms);
  assert(h2_pal_mutex_try_lock(fx->api, fx->mutex) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_pal_mutex_unlock(fx->api, fx->mutex) == H2_PAL_OK);
  w->returned = true;
  return NULL;
}

static uint32_t registered_waiters(fixture_t *fx) {
  assert(rtos_lock_mutex(&fx->cond->lock) == kNoErr);
  uint32_t n = fx->cond->waiters;
  assert(rtos_unlock_mutex(&fx->cond->lock) == kNoErr);
  return n;
}

static void wait_registered(fixture_t *fx, uint32_t at_least) {
  for (unsigned i = 0; i < 5000u && registered_waiters(fx) < at_least; ++i) {
    sleep_ms(1u);
  }
  assert(registered_waiters(fx) >= at_least);
}

static void wait_waiting(waiter_t *w) {
  for (unsigned i = 0; i < 5000u && !w->waiting; ++i) {
    sleep_ms(1u);
  }
  assert(w->waiting);
}

static void wait_returned(waiter_t *w) {
  for (unsigned i = 0; i < 5000u && !w->returned; ++i) {
    sleep_ms(1u);
  }
  assert(w->returned);
  assert(pthread_join(w->thread, NULL) == 0);
}

static void fixture_open(fixture_t *fx) {
  memset(fx, 0, sizeof(*fx));
  fx->api = h2_bk_platform_sync_api();
  const h2_pal_mutex_config_t mutex_config = {.name = "cond-mutex"};
  const h2_pal_cond_config_t cond_config = {.name = "cond"};
  assert(h2_pal_mutex_create(fx->api, &mutex_config, &fx->mutex) == H2_PAL_OK);
  assert(h2_pal_cond_create(fx->api, &cond_config, &fx->cond) == H2_PAL_OK);
}

static void fixture_close(fixture_t *fx) {
  assert(registered_waiters(fx) == 0u);
  assert(h2_pal_cond_destroy(fx->api, fx->cond) == H2_PAL_OK);
  assert(h2_pal_mutex_destroy(fx->api, fx->mutex) == H2_PAL_OK);
}

static void start(waiter_t *w, fixture_t *fx, uint32_t timeout_ms,
                  void *(*body)(void *)) {
  memset(w, 0, sizeof(*w));
  w->fx = fx;
  w->timeout_ms = timeout_ms;
  assert(pthread_create(&w->thread, NULL, body, w) == 0);
}

/* The regression: a broadcast must reach the WAIT_FOREVER waiter even though a
 * 1 ms poller keeps leaving and re-entering the wait around it. */
static void test_broadcast_reaches_parked_waiter_despite_poller(void) {
  for (unsigned round = 0; round < 200u; ++round) {
    fixture_t fx;
    fixture_open(&fx);
    waiter_t parked, poller;
    start(&poller, &fx, 1u, polling_waiter);
    start(&parked, &fx, H2_PAL_SYNC_WAIT_FOREVER, forever_waiter);
    wait_waiting(&parked);
    /* Let the poller cycle a few times around the parked waiter. */
    sleep_ms(3u);
    /* Taking the mutex here guarantees the parked waiter is registered. */
    assert(h2_pal_mutex_lock(fx.api, fx.mutex) == H2_PAL_OK);
    assert(registered_waiters(&fx) >= 1u);
    fx.done = true;
    assert(h2_pal_cond_broadcast(fx.api, fx.cond) == H2_PAL_OK);
    assert(h2_pal_mutex_unlock(fx.api, fx.mutex) == H2_PAL_OK);
    wait_returned(&parked);
    assert(parked.wakeups == 1u);
    fx.stop = true;
    wait_returned(&poller);
    fixture_close(&fx);
  }
}

/* signal wakes exactly one of two parked waiters; broadcast wakes the rest. */
static void test_signal_wakes_one_broadcast_wakes_all(void) {
  fixture_t fx;
  fixture_open(&fx);
  waiter_t a, b;
  start(&a, &fx, H2_PAL_SYNC_WAIT_FOREVER, single_waiter);
  start(&b, &fx, H2_PAL_SYNC_WAIT_FOREVER, single_waiter);
  wait_registered(&fx, 2u);

  assert(h2_pal_mutex_lock(fx.api, fx.mutex) == H2_PAL_OK);
  assert(h2_pal_cond_signal(fx.api, fx.cond) == H2_PAL_OK);
  assert(h2_pal_mutex_unlock(fx.api, fx.mutex) == H2_PAL_OK);
  for (unsigned i = 0; i < 5000u && registered_waiters(&fx) != 1u; ++i) {
    sleep_ms(1u);
  }
  sleep_ms(5u);
  assert(registered_waiters(&fx) == 1u);
  assert(a.returned != b.returned);
  /* The first waiter in line is the one signalled. */
  assert(a.returned && a.result == H2_PAL_OK);

  assert(h2_pal_mutex_lock(fx.api, fx.mutex) == H2_PAL_OK);
  assert(h2_pal_cond_broadcast(fx.api, fx.cond) == H2_PAL_OK);
  assert(h2_pal_mutex_unlock(fx.api, fx.mutex) == H2_PAL_OK);
  wait_returned(&a);
  wait_returned(&b);
  assert(b.result == H2_PAL_OK);
  fixture_close(&fx);
}

/* A wakeup with nobody registered leaves nothing behind; the next timed wait
 * times out and returns with the mutex held. */
static void test_wake_without_waiters_leaves_no_token(void) {
  fixture_t fx;
  fixture_open(&fx);
  assert(h2_pal_mutex_lock(fx.api, fx.mutex) == H2_PAL_OK);
  assert(h2_pal_cond_signal(fx.api, fx.cond) == H2_PAL_OK);
  assert(h2_pal_cond_broadcast(fx.api, fx.cond) == H2_PAL_OK);
  assert(h2_pal_cond_wait(fx.api, fx.cond, fx.mutex, 1u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(h2_pal_cond_wait(fx.api, fx.cond, fx.mutex, H2_PAL_SYNC_NO_WAIT) ==
         H2_PAL_ERR_TIMEOUT);
  assert(h2_pal_mutex_try_lock(fx.api, fx.mutex) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_pal_mutex_unlock(fx.api, fx.mutex) == H2_PAL_OK);
  assert(registered_waiters(&fx) == 0u);
  fixture_close(&fx);
}

/* destroy refuses while a waiter is registered and succeeds once it left. */
static void test_destroy_refuses_with_waiter(void) {
  fixture_t fx;
  fixture_open(&fx);
  waiter_t w;
  start(&w, &fx, H2_PAL_SYNC_WAIT_FOREVER, single_waiter);
  wait_registered(&fx, 1u);
  assert(h2_pal_cond_destroy(fx.api, fx.cond) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_mutex_lock(fx.api, fx.mutex) == H2_PAL_OK);
  assert(h2_pal_cond_signal(fx.api, fx.cond) == H2_PAL_OK);
  assert(h2_pal_mutex_unlock(fx.api, fx.mutex) == H2_PAL_OK);
  wait_returned(&w);
  assert(w.result == H2_PAL_OK);
  fixture_close(&fx);
}

/* Contract guards: a recursive mutex is rejected, and each wait parks on a
 * fresh binary semaphore rather than a shared token pool. */
static void test_wait_rejects_recursive_mutex(void) {
  fixture_t fx;
  fixture_open(&fx);
  h2_pal_mutex_t *recursive = NULL;
  const h2_pal_mutex_config_t config = {.name = "recursive",
                                        .flags = H2_PAL_MUTEX_FLAG_RECURSIVE};
  assert(h2_pal_mutex_create(fx.api, &config, &recursive) == H2_PAL_OK);
  assert(h2_pal_mutex_lock(fx.api, recursive) == H2_PAL_OK);
  unsigned before = binary_semaphores_created;
  assert(h2_pal_cond_wait(fx.api, fx.cond, recursive, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(binary_semaphores_created == before);
  assert(h2_pal_mutex_unlock(fx.api, recursive) == H2_PAL_OK);
  assert(h2_pal_mutex_destroy(fx.api, recursive) == H2_PAL_OK);

  assert(h2_pal_mutex_lock(fx.api, fx.mutex) == H2_PAL_OK);
  assert(h2_pal_cond_wait(fx.api, fx.cond, fx.mutex, 1u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(h2_pal_cond_wait(fx.api, fx.cond, fx.mutex, 1u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(binary_semaphores_created == before + 2u);
  assert(h2_pal_mutex_unlock(fx.api, fx.mutex) == H2_PAL_OK);
  fixture_close(&fx);
}

int main(void) {
  /* A lost wakeup shows up as a hang; let SIGALRM turn it into a failure. */
  alarm(120u);
  test_wake_without_waiters_leaves_no_token();
  test_wait_rejects_recursive_mutex();
  test_destroy_refuses_with_waiter();
  test_signal_wakes_one_broadcast_wakes_all();
  test_broadcast_reaches_parked_waiter_despite_poller();
  return 0;
}
