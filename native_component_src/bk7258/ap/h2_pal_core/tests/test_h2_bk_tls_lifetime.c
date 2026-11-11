#define _POSIX_C_SOURCE 200809L
#include "h2_bk_tls_lifetime.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_FDS 160u
#define TEST_WORKERS H2_BK_TLS_JOB_MAX
#define WATCHDOG_SECONDS 5

typedef enum barrier_stage {
  STAGE_NONE = 0,
  STAGE_ALLOC,
  STAGE_ROOT_FREE,
  STAGE_SETUP,
  STAGE_HANDSHAKE,
  STAGE_PUBLICATION,
  STAGE_DISPOSE,
  STAGE_CLOSE,
} barrier_stage_t;

typedef struct plan {
  barrier_stage_t block;
  bool entered;
  bool released;
  bool check_config;
  bool set_completion_time;
  uint64_t completion_time;
  h2_pal_result_t result;
  unsigned wraps;
  unsigned disposals;
  unsigned closes;
  unsigned native_io;
  size_t root_length;
  bool fail_alloc;
  unsigned root_allocs;
  unsigned root_frees;
  bool live;
  bool context;
} plan_t;

typedef struct root_allocation {
  void* pointer;
  int fd;
} root_allocation_t;

typedef struct fixture {
  h2_bk_tls_lifetime_t manager;
  pthread_mutex_t metadata;
  pthread_mutex_t events;
  pthread_cond_t changed;
  pthread_t workers[TEST_WORKERS];
  size_t worker_count;
  bool stop;
  bool expire_at_claim;
  uint32_t copy_clock_step;
  bool copy_only_clock_step;
  atomic_uint_fast64_t now_ms;
  plan_t plans[TEST_FDS];
  root_allocation_t roots[H2_BK_TLS_JOB_MAX * 2u];
} fixture_t;

typedef struct request {
  fixture_t* fixture;
  int fd;
  uint32_t timeout_ms;
  h2_pal_net_tls_config_t config;
  int output;
  h2_pal_result_t result;
  bool done;
  pthread_t thread;
} request_t;

static _Thread_local unsigned metadata_depth;
static _Thread_local bool request_thread;

static struct timespec real_deadline(unsigned milliseconds) {
  struct timespec deadline;
  assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += milliseconds / 1000u;
  deadline.tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    ++deadline.tv_sec;
    deadline.tv_nsec -= 1000000000L;
  }
  return deadline;
}

static uintptr_t enter_metadata(void* raw) {
  fixture_t* fixture = raw;
  assert(metadata_depth == 0u);
  assert(pthread_mutex_lock(&fixture->metadata) == 0);
  ++metadata_depth;
  return 7u;
}

static void leave_metadata(void* raw, uintptr_t guard) {
  fixture_t* fixture = raw;
  assert(guard == 7u && metadata_depth == 1u);
  --metadata_depth;
  assert(pthread_mutex_unlock(&fixture->metadata) == 0);
}

static uint64_t clock_ms(void* raw) {
  fixture_t* fixture = raw;
  if (request_thread && fixture->copy_clock_step != 0u) {
    bool copying = false;
    if (metadata_depth != 0u) {
      for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX; ++i)
        if (fixture->manager.jobs[i].state == H2_BK_TLS_JOB_COPYING)
          copying = true;
    }
    if (copying || (metadata_depth == 0u && !fixture->copy_only_clock_step))
      return atomic_fetch_add(&fixture->now_ms, fixture->copy_clock_step);
  }
  if (metadata_depth != 0u && request_thread && fixture->expire_at_claim) {
    for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX; ++i) {
      const h2_bk_tls_job_t* job = &fixture->manager.jobs[i];
      if (job->state == H2_BK_TLS_JOB_DONE && job->result == H2_PAL_OK &&
          job->waiter) {
        atomic_store(&fixture->now_ms, job->deadline_ms);
        break;
      }
    }
  }
  return atomic_load(&fixture->now_ms);
}

static void wait_ms(void* raw, uint32_t timeout_ms) {
  fixture_t* fixture = raw;
  assert(metadata_depth == 0u && timeout_ms > 0u && timeout_ms <= 10u);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  struct timespec deadline = real_deadline(timeout_ms);
  int result =
      pthread_cond_timedwait(&fixture->changed, &fixture->events, &deadline);
  assert(result == 0 || result == ETIMEDOUT);
  assert(pthread_mutex_unlock(&fixture->events) == 0);
}

static void wake_worker(void* raw) {
  fixture_t* fixture = raw;
  assert(metadata_depth == 0u);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  assert(pthread_cond_broadcast(&fixture->changed) == 0);
  assert(pthread_mutex_unlock(&fixture->events) == 0);
}

/* Each barrier is inside a potentially blocking native callback, never the
 * metadata guard. Fake AON advances independently of this blocked worker. */
static void native_barrier(fixture_t* fixture, int fd, barrier_stage_t stage) {
  assert(metadata_depth == 0u && fd >= 0 && (unsigned)fd < TEST_FDS);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  plan_t* plan = &fixture->plans[fd];
  if (plan->block == stage) {
    plan->entered = true;
    assert(pthread_cond_broadcast(&fixture->changed) == 0);
    struct timespec deadline = real_deadline(WATCHDOG_SECONDS * 1000u);
    while (!plan->released) {
      int result = pthread_cond_timedwait(&fixture->changed, &fixture->events,
                                          &deadline);
      assert(result == 0);
    }
  }
  assert(pthread_mutex_unlock(&fixture->events) == 0);
}

/* PEM-shaped transport bytes, not a cryptographic trust/verification fixture.
 */
static const char root_pem_block[] =
    "-----BEGIN CERTIFICATE-----\n"
    "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=\n"
    "-----END CERTIFICATE-----\n";

static uint8_t root_byte(size_t offset) {
  return (uint8_t)root_pem_block[offset % (sizeof(root_pem_block) - 1u)];
}

static void* native_alloc_root(void* raw, size_t bytes) {
  fixture_t* fixture = raw;
  assert(metadata_depth == 0u && !request_thread);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  int fd = -1;
  for (size_t i = 0u; i < TEST_FDS; ++i) {
    if (fixture->plans[i].root_length != 0u &&
        fixture->plans[i].root_length + 1u == bytes) {
      assert(fd == -1);
      fd = (int)i;
    }
  }
  assert(fd >= 0 && fixture->plans[fd].live);
  ++fixture->plans[fd].root_allocs;
  bool fail = fixture->plans[fd].fail_alloc;
  assert(pthread_mutex_unlock(&fixture->events) == 0);
  native_barrier(fixture, fd, STAGE_ALLOC);
  if (fail) return NULL;
  void* root = malloc(bytes);
  assert(root != NULL);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  size_t slot = 0u;
  while (slot < H2_BK_TLS_JOB_MAX * 2u && fixture->roots[slot].pointer != NULL)
    ++slot;
  assert(slot < H2_BK_TLS_JOB_MAX * 2u);
  fixture->roots[slot] = (root_allocation_t){.pointer = root, .fd = fd};
  assert(pthread_mutex_unlock(&fixture->events) == 0);
  return root;
}

static void native_free_root(void* raw, void* root) {
  fixture_t* fixture = raw;
  assert(metadata_depth == 0u && !request_thread && root != NULL);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  size_t slot = 0u;
  while (slot < H2_BK_TLS_JOB_MAX * 2u && fixture->roots[slot].pointer != root)
    ++slot;
  assert(slot < H2_BK_TLS_JOB_MAX * 2u);
  int fd = fixture->roots[slot].fd;
  assert(fixture->plans[fd].live);
  assert(pthread_mutex_unlock(&fixture->events) == 0);
  native_barrier(fixture, fd, STAGE_ROOT_FREE);
  free(root);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  assert(fixture->plans[fd].live);
  fixture->roots[slot].pointer = NULL;
  ++fixture->plans[fd].root_frees;
  assert(pthread_cond_broadcast(&fixture->changed) == 0);
  assert(pthread_mutex_unlock(&fixture->events) == 0);
}

static h2_pal_result_t native_wrap(void* raw, int fd,
                                   const h2_pal_net_tls_config_t* config,
                                   uint64_t deadline_ms) {
  fixture_t* fixture = raw;
  assert(metadata_depth == 0u);
  assert(config != NULL && deadline_ms > 0u);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  plan_t* plan = &fixture->plans[fd];
  assert(plan->live && !plan->context);
  ++plan->wraps;
  plan->context = true;
  assert(pthread_mutex_unlock(&fixture->events) == 0);
  native_barrier(fixture, fd, STAGE_SETUP);
  native_barrier(fixture, fd, STAGE_HANDSHAKE);
  native_barrier(fixture, fd, STAGE_PUBLICATION);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  if (plan->check_config) {
    assert(strcmp(config->server_name, "example.test") == 0);
    assert(strcmp(config->alpn, "http/1.1") == 0);
    assert(config->verify == H2_PAL_NET_TLS_VERIFY_REQUIRED);
    assert(config->root_ca_pem_len == 1024u);
    for (size_t i = 0u; i < config->root_ca_pem_len; ++i)
      assert(config->root_ca_pem[i] == (uint8_t)(i % 251u));
    assert(config->root_ca_pem[config->root_ca_pem_len] == 0u);
  }
  if (plan->root_length != 0u) {
    assert(config->root_ca_pem != NULL);
    assert(config->root_ca_pem_len == plan->root_length);
    for (size_t i = 0u; i < plan->root_length; ++i)
      assert(config->root_ca_pem[i] == root_byte(i));
    assert(config->root_ca_pem[plan->root_length] == 0u);
  }
  if (plan->set_completion_time)
    atomic_store(&fixture->now_ms, plan->completion_time);
  h2_pal_result_t result = plan->result;
  assert(pthread_mutex_unlock(&fixture->events) == 0);
  if (!h2_bk_tls_bio_canceled(&fixture->manager, fd)) {
    assert(pthread_mutex_lock(&fixture->events) == 0);
    assert(plan->live);
    ++plan->native_io;
    assert(pthread_mutex_unlock(&fixture->events) == 0);
  }
  wake_worker(fixture);
  return result;
}

static void native_dispose(void* raw, int fd) {
  fixture_t* fixture = raw;
  native_barrier(fixture, fd, STAGE_DISPOSE);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  plan_t* plan = &fixture->plans[fd];
  assert(plan->live);
  ++plan->disposals;
  plan->context = false;
  assert(pthread_cond_broadcast(&fixture->changed) == 0);
  assert(pthread_mutex_unlock(&fixture->events) == 0);
}

static void native_close(void* raw, int fd) {
  fixture_t* fixture = raw;
  native_barrier(fixture, fd, STAGE_CLOSE);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  plan_t* plan = &fixture->plans[fd];
  assert(plan->live && !plan->context);
  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX * 2u; ++i)
    assert(fixture->roots[i].pointer == NULL || fixture->roots[i].fd != fd);
  plan->live = false;
  ++plan->closes;
  assert(pthread_cond_broadcast(&fixture->changed) == 0);
  assert(pthread_mutex_unlock(&fixture->events) == 0);
}

static void* worker_main(void* raw) {
  fixture_t* fixture = raw;
  for (;;) {
    if (h2_bk_tls_worker_step(&fixture->manager)) continue;
    assert(pthread_mutex_lock(&fixture->events) == 0);
    bool stop = fixture->stop;
    if (!stop) {
      struct timespec deadline = real_deadline(1u);
      int result = pthread_cond_timedwait(&fixture->changed, &fixture->events,
                                          &deadline);
      assert(result == 0 || result == ETIMEDOUT);
    }
    assert(pthread_mutex_unlock(&fixture->events) == 0);
    if (stop) return NULL;
  }
}

static void fixture_init_with_roots(fixture_t* fixture, size_t workers,
                                    bool roots) {
  memset(fixture, 0, sizeof(*fixture));
  atomic_init(&fixture->now_ms, 0u);
  assert(pthread_mutex_init(&fixture->metadata, NULL) == 0);
  assert(pthread_mutex_init(&fixture->events, NULL) == 0);
  assert(pthread_cond_init(&fixture->changed, NULL) == 0);
  h2_bk_tls_lifetime_ops_t ops = {
      .user = fixture,
      .enter = enter_metadata,
      .leave = leave_metadata,
      .clock_ms = clock_ms,
      .wait_ms = wait_ms,
      .wake_worker = wake_worker,
      .wrap = native_wrap,
      .dispose = native_dispose,
      .close_fd = native_close,
      .alloc_root = roots ? native_alloc_root : NULL,
      .free_root = roots ? native_free_root : NULL};
  assert(h2_bk_tls_lifetime_init(&fixture->manager, &ops) == H2_PAL_OK);
  fixture->worker_count = workers;
  for (size_t i = 0u; i < workers; ++i)
    assert(pthread_create(&fixture->workers[i], NULL, worker_main, fixture) ==
           0);
  h2_bk_tls_lifetime_set_worker_ready(&fixture->manager, workers != 0u);
}

static void fixture_init(fixture_t* fixture, size_t workers) {
  fixture_init_with_roots(fixture, workers, true);
}

static void wait_closed(fixture_t* fixture, int fd, unsigned count) {
  assert(pthread_mutex_lock(&fixture->events) == 0);
  struct timespec deadline = real_deadline(WATCHDOG_SECONDS * 1000u);
  while (fixture->plans[fd].closes < count) {
    int result =
        pthread_cond_timedwait(&fixture->changed, &fixture->events, &deadline);
    assert(result == 0);
  }
  assert(fixture->plans[fd].closes == count);
  assert(pthread_mutex_unlock(&fixture->events) == 0);
  /* A native close may just have returned; wait for metadata retirement. */
  for (;;) {
    h2_bk_tls_tcp_ref_t ref;
    bool tls;
    h2_pal_result_t result =
        h2_bk_tls_tcp_acquire(&fixture->manager, fd, &ref, &tls);
    if (result == H2_PAL_ERR_NOT_FOUND) break;
    assert(result == H2_PAL_ERR_CLOSED);
    struct timespec now;
    assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
    assert(now.tv_sec < deadline.tv_sec ||
           (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
    struct timespec pause = {.tv_nsec = 100000L};
    assert(nanosleep(&pause, NULL) == 0);
  }
}

static void fixture_destroy(fixture_t* fixture) {
  for (size_t fd = 0u; fd < TEST_FDS; ++fd) {
    h2_pal_result_t result = h2_bk_tls_tcp_close(&fixture->manager, (int)fd);
    assert(result == H2_PAL_OK || result == H2_PAL_ERR_NOT_FOUND);
  }
  if (fixture->worker_count == 0u)
    while (h2_bk_tls_worker_step(&fixture->manager)) {
    }
  for (size_t fd = 0u; fd < TEST_FDS; ++fd) {
    assert(pthread_mutex_lock(&fixture->events) == 0);
    bool live = fixture->plans[fd].live;
    unsigned closed = fixture->plans[fd].closes;
    assert(pthread_mutex_unlock(&fixture->events) == 0);
    if (live) wait_closed(fixture, (int)fd, closed + 1u);
  }
  assert(pthread_mutex_lock(&fixture->events) == 0);
  fixture->stop = true;
  assert(pthread_cond_broadcast(&fixture->changed) == 0);
  assert(pthread_mutex_unlock(&fixture->events) == 0);
  for (size_t i = 0u; i < fixture->worker_count; ++i)
    assert(pthread_join(fixture->workers[i], NULL) == 0);
  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX * 2u; ++i)
    assert(fixture->roots[i].pointer == NULL);
  assert(pthread_cond_destroy(&fixture->changed) == 0);
  assert(pthread_mutex_destroy(&fixture->events) == 0);
  assert(pthread_mutex_destroy(&fixture->metadata) == 0);
}

static void open_fd(fixture_t* fixture, int fd, barrier_stage_t block) {
  assert(h2_bk_tls_tcp_register(&fixture->manager, fd) == H2_PAL_OK);
  assert(pthread_mutex_lock(&fixture->events) == 0);
  assert(!fixture->plans[fd].live);
  fixture->plans[fd].live = true;
  fixture->plans[fd].block = block;
  fixture->plans[fd].entered = false;
  fixture->plans[fd].released = false;
  fixture->plans[fd].root_length = 0u;
  fixture->plans[fd].fail_alloc = false;
  assert(pthread_mutex_unlock(&fixture->events) == 0);
}

static void set_time(fixture_t* fixture, uint64_t now_ms) {
  atomic_store(&fixture->now_ms, now_ms);
  wake_worker(fixture);
}

static void wait_entered(fixture_t* fixture, int fd) {
  assert(pthread_mutex_lock(&fixture->events) == 0);
  struct timespec deadline = real_deadline(WATCHDOG_SECONDS * 1000u);
  while (!fixture->plans[fd].entered) {
    int result =
        pthread_cond_timedwait(&fixture->changed, &fixture->events, &deadline);
    assert(result == 0);
  }
  assert(!fixture->plans[fd].released);
  assert(pthread_mutex_unlock(&fixture->events) == 0);
}

static void release_barrier(fixture_t* fixture, int fd) {
  assert(pthread_mutex_lock(&fixture->events) == 0);
  fixture->plans[fd].released = true;
  assert(pthread_cond_broadcast(&fixture->changed) == 0);
  assert(pthread_mutex_unlock(&fixture->events) == 0);
}

static void* request_main(void* raw) {
  request_t* request = raw;
  request_thread = true;
  request->result = h2_bk_tls_wrap_bounded(
      &request->fixture->manager, request->fd, &request->config,
      request->timeout_ms, &request->output);
  assert(pthread_mutex_lock(&request->fixture->events) == 0);
  request->done = true;
  assert(pthread_cond_broadcast(&request->fixture->changed) == 0);
  assert(pthread_mutex_unlock(&request->fixture->events) == 0);
  return NULL;
}

static void start_request(request_t* request, fixture_t* fixture, int fd,
                          uint32_t timeout_ms) {
  *request = (request_t){.fixture = fixture,
                         .fd = fd,
                         .timeout_ms = timeout_ms,
                         .config = {.server_name = "example.test",
                                    .alpn = "http/1.1",
                                    .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED},
                         .output = 123};
}

static void launch_request(request_t* request) {
  assert(pthread_create(&request->thread, NULL, request_main, request) == 0);
}

static void finish_request(request_t* request, h2_pal_result_t expected) {
  assert(pthread_mutex_lock(&request->fixture->events) == 0);
  struct timespec deadline = real_deadline(WATCHDOG_SECONDS * 1000u);
  while (!request->done) {
    int result = pthread_cond_timedwait(&request->fixture->changed,
                                        &request->fixture->events, &deadline);
    assert(result == 0);
  }
  assert(pthread_mutex_unlock(&request->fixture->events) == 0);
  assert(pthread_join(request->thread, NULL) == 0);
  assert(request->result == expected);
  assert(request->output == (expected == H2_PAL_OK ? request->fd : -1));
}

static void assert_io_denied(fixture_t* fixture, int fd) {
  h2_bk_tls_tcp_ref_t ref;
  bool tls = true;
  assert(h2_bk_tls_tcp_acquire(&fixture->manager, fd, &ref, &tls) ==
         H2_PAL_ERR_CLOSED);
  assert(!tls && ref.generation == 0u);
  assert(h2_bk_tls_legacy_begin(&fixture->manager, fd, &ref) ==
         H2_PAL_ERR_INVALID_STATE);
}

static void test_blocked_wrap(barrier_stage_t stage) {
  fixture_t fixture;
  fixture_init(&fixture, 2u);
  open_fd(&fixture, 4, stage);
  request_t request;
  start_request(&request, &fixture, 4, 20u);
  char* hostname = malloc(32u);
  char* alpn = malloc(16u);
  uint8_t* ca = malloc(1024u);
  assert(hostname != NULL && alpn != NULL && ca != NULL);
  strcpy(hostname, "example.test");
  strcpy(alpn, "http/1.1");
  for (size_t i = 0u; i < 1024u; ++i) ca[i] = (uint8_t)(i % 251u);
  request.config.server_name = hostname;
  request.config.alpn = alpn;
  request.config.root_ca_pem = ca;
  request.config.root_ca_pem_len = 1024u;
  fixture.plans[4].check_config = true;
  launch_request(&request);
  wait_entered(&fixture, 4);
  assert_io_denied(&fixture, 4);
  set_time(&fixture, 20u);
  finish_request(&request, H2_PAL_ERR_TIMEOUT);
  /* Timeout precedes native completion. Destroy every borrowed input now. */
  memset(hostname, 'x', 32u);
  memset(alpn, 'x', 16u);
  memset(ca, 0xff, 1024u);
  free(hostname);
  free(alpn);
  free(ca);
  memset(&request.config, 0xa5, sizeof(request.config));
  request.output = 987;
  assert(h2_bk_tls_bio_canceled(&fixture.manager, 4));
  assert_io_denied(&fixture, 4);
  assert(h2_bk_tls_tcp_close(&fixture.manager, 4) == H2_PAL_OK);
  assert(h2_bk_tls_tcp_close(&fixture.manager, 4) == H2_PAL_OK);
  assert(h2_bk_tls_tcp_register(&fixture.manager, 4) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(fixture.plans[4].closes == 0u);
  release_barrier(&fixture, 4);
  wait_closed(&fixture, 4, 1u);
  assert(fixture.plans[4].disposals == 1u);
  assert(fixture.plans[4].native_io == 0u);
  assert(request.output == 987); /* No late writes through caller output. */
  open_fd(&fixture, 4, STAGE_NONE);
  h2_bk_tls_tcp_ref_t ref;
  bool tls;
  assert(h2_bk_tls_tcp_acquire(&fixture.manager, 4, &ref, &tls) == H2_PAL_OK);
  assert(!tls);
  assert(h2_bk_tls_tcp_release(&fixture.manager, &ref) == H2_PAL_OK);
  fixture_destroy(&fixture);
  assert(fixture.plans[4].closes == 2u);
}

static void test_deadline_boundary(uint64_t completed_at,
                                   h2_pal_result_t expected) {
  fixture_t fixture;
  fixture_init(&fixture, 2u);
  open_fd(&fixture, 5, STAGE_PUBLICATION);
  fixture.plans[5].set_completion_time = true;
  fixture.plans[5].completion_time = completed_at;
  request_t request;
  start_request(&request, &fixture, 5, 10u);
  launch_request(&request);
  wait_entered(&fixture, 5);
  release_barrier(&fixture, 5);
  finish_request(&request, expected);
  if (expected == H2_PAL_OK) {
    set_time(&fixture, 1000u);
    assert(!h2_bk_tls_bio_canceled(&fixture.manager, 5));
    h2_bk_tls_tcp_ref_t ref;
    bool tls;
    assert(h2_bk_tls_tcp_acquire(&fixture.manager, 5, &ref, &tls) == H2_PAL_OK);
    assert(tls);
    assert(h2_bk_tls_tcp_release(&fixture.manager, &ref) == H2_PAL_OK);
  } else {
    assert_io_denied(&fixture, 5);
  }
  fixture_destroy(&fixture);
  assert(fixture.plans[5].closes == 1u);
  assert(fixture.plans[5].disposals == 1u);
}

static void test_parallel_success_and_active_close(void) {
  fixture_t fixture;
  fixture_init(&fixture, TEST_WORKERS);
  open_fd(&fixture, 6, STAGE_HANDSHAKE);
  request_t slow;
  start_request(&slow, &fixture, 6, 20u);
  launch_request(&slow);
  wait_entered(&fixture, 6);
  request_t good[H2_BK_TLS_JOB_MAX - 1u];
  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX - 1u; ++i) {
    int fd = 7 + (int)i;
    open_fd(&fixture, fd, STAGE_HANDSHAKE);
    start_request(&good[i], &fixture, fd, 20u);
    launch_request(&good[i]);
  }
  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX - 1u; ++i)
    wait_entered(&fixture, 7 + (int)i);
  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX - 1u; ++i)
    release_barrier(&fixture, 7 + (int)i);
  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX - 1u; ++i)
    finish_request(&good[i], H2_PAL_OK);
  open_fd(&fixture, 14, STAGE_NONE);
  int output;
  assert(h2_bk_tls_wrap_bounded(&fixture.manager, 14, &good[0].config, 20u,
                                &output) == H2_PAL_ERR_NO_SPACE);

  /* ACTIVE close is also deferred, and in-flight TLS I/O pins its context. */
  h2_bk_tls_tcp_ref_t refs[2];
  bool tls;
  for (size_t i = 0u; i < 2u; ++i)
    assert(h2_bk_tls_tcp_acquire(&fixture.manager, 7, &refs[i], &tls) ==
               H2_PAL_OK &&
           tls);
  assert(h2_bk_tls_tcp_close(&fixture.manager, 7) == H2_PAL_OK);
  assert_io_denied(&fixture, 7);
  assert(h2_bk_tls_tcp_release(&fixture.manager, &refs[0]) == H2_PAL_OK);
  assert(pthread_mutex_lock(&fixture.events) == 0);
  assert(fixture.plans[7].closes == 0u && fixture.plans[7].disposals == 0u);
  assert(pthread_mutex_unlock(&fixture.events) == 0);
  assert(h2_bk_tls_tcp_release(&fixture.manager, &refs[1]) == H2_PAL_OK);
  wait_closed(&fixture, 7, 1u);
  assert(h2_bk_tls_tcp_release(&fixture.manager, &refs[1]) ==
         H2_PAL_ERR_INVALID_STATE);
  request_t replacement;
  start_request(&replacement, &fixture, 14, 20u);
  launch_request(&replacement);
  finish_request(&replacement, H2_PAL_OK);

  set_time(&fixture, 20u);
  finish_request(&slow, H2_PAL_ERR_TIMEOUT);
  release_barrier(&fixture, 6);
  fixture_destroy(&fixture);
  for (int fd = 6; fd < 15; ++fd) {
    assert(fixture.plans[fd].closes == 1u);
    assert(fixture.plans[fd].disposals == 1u);
    assert(fixture.plans[fd].wraps == 1u);
  }
}

static void test_blocked_reaper(barrier_stage_t stage) {
  fixture_t fixture;
  fixture_init(&fixture, 3u);
  open_fd(&fixture, 15, stage);
  request_t active;
  start_request(&active, &fixture, 15, 20u);
  launch_request(&active);
  finish_request(&active, H2_PAL_OK);
  assert(h2_bk_tls_tcp_close(&fixture.manager, 15) == H2_PAL_OK);
  wait_entered(&fixture, 15);
  /* Other native operations and timeout publication cannot need this guard. */
  open_fd(&fixture, 16, STAGE_SETUP);
  request_t blocked;
  start_request(&blocked, &fixture, 16, 20u);
  launch_request(&blocked);
  wait_entered(&fixture, 16);
  set_time(&fixture, 20u);
  finish_request(&blocked, H2_PAL_ERR_TIMEOUT);
  assert(h2_bk_tls_tcp_close(&fixture.manager, 16) == H2_PAL_OK);
  assert(h2_bk_tls_tcp_register(&fixture.manager, 15) ==
         H2_PAL_ERR_INVALID_STATE);
  release_barrier(&fixture, 16);
  wait_closed(&fixture, 16, 1u);
  assert(fixture.plans[15].closes == 0u);
  release_barrier(&fixture, 15);
  wait_closed(&fixture, 15, 1u);
  fixture_destroy(&fixture);
}

static void wait_queued(fixture_t* fixture, size_t count) {
  struct timespec deadline = real_deadline(WATCHDOG_SECONDS * 1000u);
  for (;;) {
    uintptr_t guard = enter_metadata(fixture);
    size_t queued = 0u;
    for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX; ++i)
      if (fixture->manager.jobs[i].state == H2_BK_TLS_JOB_QUEUED) ++queued;
    leave_metadata(fixture, guard);
    if (queued == count) return;
    struct timespec now;
    assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
    assert(now.tv_sec < deadline.tv_sec ||
           (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
    struct timespec pause = {.tv_nsec = 100000L};
    assert(nanosleep(&pause, NULL) == 0);
  }
}

static void test_queued_cancel_and_capacity(void) {
  fixture_t fixture;
  fixture_init(&fixture, 0u);
  open_fd(&fixture, 20, STAGE_NONE);
  int out = 123;
  h2_pal_net_tls_config_t config = {.server_name = "example.test"};
  assert(h2_bk_tls_wrap_bounded(&fixture.manager, 20, &config, 10u, &out) ==
             H2_PAL_ERR_UNSUPPORTED &&
         out == -1);
  /* Admit a queue before starting the separately managed worker. */
  h2_bk_tls_lifetime_set_worker_ready(&fixture.manager, true);
  request_t queued[H2_BK_TLS_JOB_MAX];
  for (size_t i = 0u; i < H2_BK_TLS_JOB_MAX; ++i) {
    int fd = 21 + (int)i;
    open_fd(&fixture, fd, STAGE_NONE);
    start_request(&queued[i], &fixture, fd, 10u);
    launch_request(&queued[i]);
  }
  wait_queued(&fixture, H2_BK_TLS_JOB_MAX);
  assert(h2_bk_tls_wrap_bounded(&fixture.manager, 20, &config, 10u, &out) ==
             H2_PAL_ERR_NO_SPACE &&
         out == -1);
  h2_bk_tls_tcp_ref_t ref;
  bool tls;
  assert(h2_bk_tls_tcp_acquire(&fixture.manager, 20, &ref, &tls) == H2_PAL_OK &&
         !tls);
  assert(h2_bk_tls_tcp_release(&fixture.manager, &ref) == H2_PAL_OK);
  assert(h2_bk_tls_tcp_close(&fixture.manager, 21) == H2_PAL_OK);
  finish_request(&queued[0], H2_PAL_ERR_CLOSED);
  set_time(&fixture, 10u);
  for (size_t i = 1u; i < H2_BK_TLS_JOB_MAX; ++i) {
    finish_request(&queued[i], H2_PAL_ERR_TIMEOUT);
    assert_io_denied(&fixture, 21 + (int)i);
  }
  while (h2_bk_tls_worker_step(&fixture.manager)) {
  }
  for (int fd = 21; fd < 29; ++fd) assert(fixture.plans[fd].wraps == 0u);
  /* Failure did not transfer native ownership, even after queued work drained.
   */
  assert(fixture.plans[22].live && fixture.plans[22].closes == 0u);
  fixture_destroy(&fixture);
}

static void test_legacy_and_plain_refs(void) {
  fixture_t fixture;
  fixture_init(&fixture, 2u);
  open_fd(&fixture, 30, STAGE_NONE);
  h2_bk_tls_tcp_ref_t io, legacy;
  bool tls;
  assert(h2_bk_tls_tcp_acquire(&fixture.manager, 30, &io, &tls) == H2_PAL_OK &&
         !tls);
  assert(h2_bk_tls_legacy_begin(&fixture.manager, 30, &legacy) ==
         H2_PAL_ERR_BUSY);
  assert(h2_bk_tls_tcp_release(&fixture.manager, &io) == H2_PAL_OK);
  assert(h2_bk_tls_legacy_begin(&fixture.manager, 30, &legacy) == H2_PAL_OK);
  assert_io_denied(&fixture, 30);
  assert(!h2_bk_tls_bio_canceled(&fixture.manager, 30));
  assert(h2_bk_tls_tcp_close(&fixture.manager, 30) == H2_PAL_OK);
  assert(h2_bk_tls_bio_canceled(&fixture.manager, 30));
  assert(h2_bk_tls_legacy_finish(&fixture.manager, &legacy, H2_PAL_OK) ==
         H2_PAL_ERR_CLOSED);
  wait_closed(&fixture, 30, 1u);
  assert(fixture.plans[30].disposals == 1u);

  open_fd(&fixture, 31, STAGE_NONE);
  assert(h2_bk_tls_legacy_begin(&fixture.manager, 31, &legacy) == H2_PAL_OK);
  assert(h2_bk_tls_legacy_finish(&fixture.manager, &legacy,
                                 H2_PAL_ERR_TLS_VERIFY) ==
         H2_PAL_ERR_TLS_VERIFY);
  assert_io_denied(&fixture, 31);
  open_fd(&fixture, 32, STAGE_NONE);
  assert(h2_bk_tls_tcp_acquire(&fixture.manager, 32, &io, &tls) == H2_PAL_OK);
  h2_bk_tls_tcp_ref_t old_ref = io;
  assert(h2_bk_tls_tcp_close(&fixture.manager, 32) == H2_PAL_OK);
  assert(h2_bk_tls_tcp_register(&fixture.manager, 32) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_bk_tls_tcp_release(&fixture.manager, &io) == H2_PAL_OK);
  wait_closed(&fixture, 32, 1u);
  open_fd(&fixture, 32, STAGE_NONE);
  assert(h2_bk_tls_tcp_release(&fixture.manager, &old_ref) ==
         H2_PAL_ERR_INVALID_STATE);

  open_fd(&fixture, 33, STAGE_NONE);
  assert(h2_bk_tls_legacy_begin(&fixture.manager, 33, &legacy) == H2_PAL_OK);
  assert(h2_bk_tls_legacy_finish(&fixture.manager, &legacy, H2_PAL_OK) ==
         H2_PAL_OK);
  set_time(&fixture, UINT64_MAX);
  assert(!h2_bk_tls_bio_canceled(&fixture.manager, 33));
  assert(h2_bk_tls_tcp_acquire(&fixture.manager, 33, &io, &tls) == H2_PAL_OK &&
         tls);
  assert(h2_bk_tls_tcp_close(&fixture.manager, 33) == H2_PAL_OK);
  assert(h2_bk_tls_bio_canceled(&fixture.manager, 33));
  assert(h2_bk_tls_tcp_release(&fixture.manager, &io) == H2_PAL_OK);
  wait_closed(&fixture, 33, 1u);
  fixture_destroy(&fixture);
}

static void test_validation_and_registry_capacity(void) {
  fixture_t fixture;
  fixture_init(&fixture, 0u);
  h2_pal_net_tls_config_t config = {.server_name = "example.test"};
  int out = 123;
  assert(h2_bk_tls_lifetime_init(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  h2_bk_tls_lifetime_t rejected;
  h2_bk_tls_lifetime_ops_t incomplete = fixture.manager.ops;
  incomplete.free_root = NULL;
  assert(h2_bk_tls_lifetime_init(&rejected, &incomplete) ==
         H2_PAL_ERR_INVALID_ARG);
  incomplete = fixture.manager.ops;
  incomplete.alloc_root = NULL;
  assert(h2_bk_tls_lifetime_init(&rejected, &incomplete) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_tls_wrap_bounded(&fixture.manager, 3, &config, 0u, &out) ==
             H2_PAL_ERR_INVALID_ARG &&
         out == -1);
  uint8_t ca = 0u;
  config.root_ca_pem = &ca;
  config.root_ca_pem_len = H2_BK_TLS_CA_CAPACITY + 1u;
  assert(h2_bk_tls_wrap_bounded(&fixture.manager, 3, &config, 10u, &out) ==
         H2_PAL_ERR_UNSUPPORTED);
  config.root_ca_pem_len = SIZE_MAX;
  assert(h2_bk_tls_wrap_bounded(&fixture.manager, 3, &config, 10u, &out) ==
         H2_PAL_ERR_INVALID_ARG);
  config.root_ca_pem_len = 0u;
  config.alpn = "1234567890123456";
  assert(h2_bk_tls_wrap_bounded(&fixture.manager, 3, &config, 10u, &out) ==
         H2_PAL_ERR_INVALID_ARG);
  config.alpn = NULL;
  char hostname[H2_BK_TLS_HOST_CAPACITY];
  memset(hostname, 'x', sizeof(hostname));
  config.server_name = hostname;
  assert(h2_bk_tls_wrap_bounded(&fixture.manager, 3, &config, 10u, &out) ==
         H2_PAL_ERR_INVALID_ARG);
  for (size_t i = 0u; i < H2_BK_TLS_TCP_MAX; ++i)
    open_fd(&fixture, (int)i, STAGE_NONE);
  assert(h2_bk_tls_tcp_register(&fixture.manager, 100) == H2_PAL_ERR_NO_SPACE);
  assert(h2_bk_tls_tcp_register(&fixture.manager, 0) ==
         H2_PAL_ERR_INVALID_STATE);
  fixture_destroy(&fixture);
}

static void test_caller_claim_expiry_and_running_close(void) {
  fixture_t fixture;
  fixture_init(&fixture, 2u);
  fixture.expire_at_claim = true;
  open_fd(&fixture, 40, STAGE_NONE);
  request_t request;
  start_request(&request, &fixture, 40, 20u);
  launch_request(&request);
  finish_request(&request, H2_PAL_ERR_TIMEOUT);
  assert(atomic_load(&fixture.now_ms) == 20u);
  assert_io_denied(&fixture, 40);
  fixture_destroy(&fixture);

  fixture_init(&fixture, 2u);
  open_fd(&fixture, 41, STAGE_HANDSHAKE);
  start_request(&request, &fixture, 41, UINT32_MAX);
  launch_request(&request);
  wait_entered(&fixture, 41);
  assert(h2_bk_tls_tcp_close(&fixture.manager, 41) == H2_PAL_OK);
  finish_request(&request, H2_PAL_ERR_CLOSED);
  assert(fixture.plans[41].closes == 0u);
  release_barrier(&fixture, 41);
  wait_closed(&fixture, 41, 1u);
  fixture_destroy(&fixture);
}

static void test_worker_deadline_wait(void) {
  fixture_t fixture;
  fixture_init(&fixture, 0u);
  assert(h2_bk_tls_worker_wait_ms(&fixture.manager) == UINT32_MAX);
  /* Isolate wait calculation using COPY_READY and DONE metadata, then discard
   * it before running lifecycle actions. Both require expiry rechecks. */
  uintptr_t guard = enter_metadata(&fixture);
  fixture.manager.jobs[0].state = H2_BK_TLS_JOB_COPY_READY;
  fixture.manager.jobs[0].waiter = true;
  fixture.manager.jobs[0].result = H2_PAL_ERR_WOULD_BLOCK;
  fixture.manager.jobs[0].deadline_ms = 50u;
  fixture.manager.jobs[1].state = H2_BK_TLS_JOB_DONE;
  fixture.manager.jobs[1].waiter = true;
  fixture.manager.jobs[1].result = H2_PAL_OK;
  fixture.manager.jobs[1].deadline_ms = 30u;
  leave_metadata(&fixture, guard);
  assert(h2_bk_tls_worker_wait_ms(&fixture.manager) == 30u);
  set_time(&fixture, 25u);
  assert(h2_bk_tls_worker_wait_ms(&fixture.manager) == 5u);
  set_time(&fixture, 30u);
  assert(h2_bk_tls_worker_wait_ms(&fixture.manager) == 0u);
  guard = enter_metadata(&fixture);
  fixture.manager.jobs[1].abandoned = true;
  leave_metadata(&fixture, guard);
  assert(h2_bk_tls_worker_wait_ms(&fixture.manager) == 20u);
  guard = enter_metadata(&fixture);
  fixture.manager.jobs[0].deadline_ms = UINT64_MAX;
  leave_metadata(&fixture, guard);
  assert(h2_bk_tls_worker_wait_ms(&fixture.manager) == UINT32_MAX - 1u);
  guard = enter_metadata(&fixture);
  fixture.manager.jobs[0].state = H2_BK_TLS_JOB_ACTIVE;
  leave_metadata(&fixture, guard);
  assert(h2_bk_tls_worker_wait_ms(&fixture.manager) == UINT32_MAX);
  guard = enter_metadata(&fixture);
  fixture.manager.jobs[0].state = H2_BK_TLS_JOB_FREE;
  fixture.manager.jobs[1].state = H2_BK_TLS_JOB_FREE;
  leave_metadata(&fixture, guard);
  fixture_destroy(&fixture);
}

static void test_copy_budget_and_native_error(void) {
  fixture_t fixture;
  fixture_init(&fixture, 2u);
  fixture.copy_clock_step = 1u;
  open_fd(&fixture, 42, STAGE_NONE);
  uint8_t* ca = malloc(H2_BK_TLS_CA_CAPACITY);
  assert(ca != NULL);
  memset(ca, 0xa5, H2_BK_TLS_CA_CAPACITY);
  request_t request;
  start_request(&request, &fixture, 42, 40u);
  request.config.root_ca_pem = ca;
  request.config.root_ca_pem_len = H2_BK_TLS_CA_CAPACITY;
  launch_request(&request);
  finish_request(&request, H2_PAL_ERR_TIMEOUT);
  free(ca);
  assert(atomic_load(&fixture.now_ms) >= 40u);
  assert(fixture.plans[42].wraps == 0u);
  assert_io_denied(&fixture, 42);
  fixture_destroy(&fixture);

  fixture_init(&fixture, 2u);
  open_fd(&fixture, 43, STAGE_NONE);
  fixture.plans[43].result = H2_PAL_ERR_TLS_VERIFY;
  start_request(&request, &fixture, 43, 20u);
  launch_request(&request);
  finish_request(&request, H2_PAL_ERR_TLS_VERIFY);
  assert_io_denied(&fixture, 43);
  assert(fixture.plans[43].closes == 0u);
  fixture_destroy(&fixture);
}

static size_t large_root_length(int fd) {
  size_t length = (sizeof(root_pem_block) - 1u) * (1024u + (unsigned)fd);
  assert(length > 64u * 1024u);
  return length;
}

static uint8_t* prepare_large_root(request_t* request, size_t length) {
  uint8_t* root = malloc(length);
  assert(root != NULL);
  for (size_t i = 0u; i < length; ++i) root[i] = root_byte(i);
  fixture_t* fixture = request->fixture;
  assert(pthread_mutex_lock(&fixture->events) == 0);
  fixture->plans[request->fd].root_length = length;
  assert(pthread_mutex_unlock(&fixture->events) == 0);
  request->config.root_ca_pem = root;
  request->config.root_ca_pem_len = length;
  return root;
}

static void wait_root_freed(fixture_t* fixture, int fd) {
  assert(pthread_mutex_lock(&fixture->events) == 0);
  struct timespec deadline = real_deadline(WATCHDOG_SECONDS * 1000u);
  while (fixture->plans[fd].root_frees == 0u) {
    int result =
        pthread_cond_timedwait(&fixture->changed, &fixture->events, &deadline);
    assert(result == 0);
  }
  assert(fixture->plans[fd].root_frees == 1u);
  assert(pthread_mutex_unlock(&fixture->events) == 0);
}

static void test_large_root_success(void) {
  fixture_t fixture;
  fixture_init(&fixture, TEST_WORKERS);
  open_fd(&fixture, 50, STAGE_NONE);
  request_t request;
  start_request(&request, &fixture, 50, 100u);
  uint8_t* root = prepare_large_root(&request, large_root_length(50));
  launch_request(&request);
  finish_request(&request, H2_PAL_OK);
  memset(root, 0xff, request.config.root_ca_pem_len);
  free(root);
  memset(&request.config, 0xa5, sizeof(request.config));
  wait_root_freed(&fixture, 50);
  set_time(&fixture, 1000u);
  assert(!h2_bk_tls_bio_canceled(&fixture.manager, 50));
  h2_bk_tls_tcp_ref_t ref;
  bool tls;
  assert(h2_bk_tls_tcp_acquire(&fixture.manager, 50, &ref, &tls) == H2_PAL_OK &&
         tls);
  assert(h2_bk_tls_tcp_release(&fixture.manager, &ref) == H2_PAL_OK);
  fixture_destroy(&fixture);
  assert(fixture.plans[50].root_allocs == 1u);
  assert(fixture.plans[50].root_frees == 1u);
  assert(fixture.plans[50].wraps == 1u);
  assert(fixture.plans[50].closes == 1u);
}

static void test_large_allocation_cancel(bool close_during_allocation) {
  fixture_t fixture;
  fixture_init(&fixture, TEST_WORKERS);
  open_fd(&fixture, 51, STAGE_ALLOC);
  request_t request;
  start_request(&request, &fixture, 51, 20u);
  uint8_t* root = prepare_large_root(&request, large_root_length(51));
  launch_request(&request);
  wait_entered(&fixture, 51);
  /* A blocked large allocation must not occupy other workers or the guard. */
  open_fd(&fixture, 52, STAGE_NONE);
  request_t large;
  start_request(&large, &fixture, 52, 20u);
  uint8_t* other_root = prepare_large_root(&large, large_root_length(52));
  launch_request(&large);
  open_fd(&fixture, 60, STAGE_NONE);
  request_t small;
  start_request(&small, &fixture, 60, 20u);
  launch_request(&small);
  finish_request(&large, H2_PAL_OK);
  finish_request(&small, H2_PAL_OK);
  free(other_root);
  if (close_during_allocation) {
    assert(h2_bk_tls_tcp_close(&fixture.manager, 51) == H2_PAL_OK);
    finish_request(&request, H2_PAL_ERR_CLOSED);
  } else {
    set_time(&fixture, 20u);
    finish_request(&request, H2_PAL_ERR_TIMEOUT);
    assert_io_denied(&fixture, 51);
    assert(h2_bk_tls_tcp_close(&fixture.manager, 51) == H2_PAL_OK);
  }
  /* Allocator is still blocked and has received no borrowed PEM pointer. */
  free(root);
  memset(&request.config, 0xa5, sizeof(request.config));
  request.output = 987;
  assert(h2_bk_tls_tcp_register(&fixture.manager, 51) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(fixture.plans[51].closes == 0u);
  release_barrier(&fixture, 51);
  wait_closed(&fixture, 51, 1u);
  assert(fixture.plans[51].root_allocs == 1u);
  assert(fixture.plans[51].root_frees == 1u);
  assert(fixture.plans[51].wraps == 0u);
  assert(request.output == 987);
  open_fd(&fixture, 51, STAGE_NONE);
  request_t reused;
  start_request(&reused, &fixture, 51, 20u);
  launch_request(&reused);
  finish_request(&reused, H2_PAL_OK);
  fixture_destroy(&fixture);
  assert(fixture.plans[51].closes == 2u);
}

static void test_large_allocation_failure_and_missing_ops(void) {
  fixture_t fixture;
  fixture_init(&fixture, 2u);
  open_fd(&fixture, 53, STAGE_NONE);
  request_t request;
  start_request(&request, &fixture, 53, 20u);
  uint8_t* root = prepare_large_root(&request, large_root_length(53));
  fixture.plans[53].fail_alloc = true;
  launch_request(&request);
  finish_request(&request, H2_PAL_ERR_NO_MEMORY);
  free(root);
  assert_io_denied(&fixture, 53);
  assert(fixture.plans[53].root_allocs == 1u);
  assert(fixture.plans[53].root_frees == 0u);
  assert(fixture.plans[53].wraps == 0u);
  assert(fixture.plans[53].closes == 0u);
  fixture_destroy(&fixture);

  fixture_init_with_roots(&fixture, 2u, false);
  open_fd(&fixture, 53, STAGE_NONE);
  start_request(&request, &fixture, 53, 20u);
  root = prepare_large_root(&request, large_root_length(53));
  launch_request(&request);
  finish_request(&request, H2_PAL_ERR_UNSUPPORTED);
  free(root);
  /* Unsupported ownership rejection never accepted or poisoned this socket. */
  h2_bk_tls_tcp_ref_t ref;
  bool tls;
  assert(h2_bk_tls_tcp_acquire(&fixture.manager, 53, &ref, &tls) == H2_PAL_OK &&
         !tls);
  assert(h2_bk_tls_tcp_release(&fixture.manager, &ref) == H2_PAL_OK);
  assert(pthread_mutex_lock(&fixture.events) == 0);
  fixture.plans[53].root_length = 0u;
  assert(pthread_mutex_unlock(&fixture.events) == 0);
  start_request(&request, &fixture, 53, 20u);
  launch_request(&request);
  finish_request(&request, H2_PAL_OK);
  fixture_destroy(&fixture);
}

static void wait_job_state(fixture_t* fixture, int fd,
                           h2_bk_tls_job_state_t state) {
  struct timespec deadline = real_deadline(WATCHDOG_SECONDS * 1000u);
  for (;;) {
    uintptr_t guard = enter_metadata(fixture);
    bool copying = false;
    for (size_t i = 0u; i < H2_BK_TLS_TCP_MAX; ++i) {
      const h2_bk_tls_tcp_record_t* tcp = &fixture->manager.tcp[i];
      if (tcp->fd == fd && tcp->job >= 0)
        copying = fixture->manager.jobs[tcp->job].state == state;
    }
    leave_metadata(fixture, guard);
    if (copying) return;
    struct timespec now;
    assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
    assert(now.tv_sec < deadline.tv_sec ||
           (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
  }
}

static void test_large_copy_cancel(bool close_during_copy) {
  fixture_t fixture;
  fixture_init(&fixture, 3u);
  open_fd(&fixture, 54, STAGE_NONE);
  request_t request;
  start_request(&request, &fixture, 54, close_during_copy ? UINT32_MAX : 40u);
  size_t length = large_root_length(54);
  if (close_during_copy)
    length = (32u * 1024u * 1024u / (sizeof(root_pem_block) - 1u)) *
             (sizeof(root_pem_block) - 1u);
  else {
    fixture.copy_clock_step = 1u;
    fixture.copy_only_clock_step = true;
  }
  uint8_t* root = prepare_large_root(&request, length);
  launch_request(&request);
  if (close_during_copy) {
    wait_job_state(&fixture, 54, H2_BK_TLS_JOB_COPYING);
    assert(h2_bk_tls_tcp_close(&fixture.manager, 54) == H2_PAL_OK);
    finish_request(&request, H2_PAL_ERR_CLOSED);
  } else {
    finish_request(&request, H2_PAL_ERR_TIMEOUT);
    assert(atomic_load(&fixture.now_ms) >= 40u);
    assert_io_denied(&fixture, 54);
    assert(h2_bk_tls_tcp_close(&fixture.manager, 54) == H2_PAL_OK);
  }
  free(root);
  wait_closed(&fixture, 54, 1u);
  assert(fixture.plans[54].root_allocs == 1u);
  assert(fixture.plans[54].root_frees == 1u);
  assert(fixture.plans[54].wraps == 0u);
  fixture_destroy(&fixture);
}

static void test_large_input_after_wrap_timeout(void) {
  fixture_t fixture;
  fixture_init(&fixture, 3u);
  open_fd(&fixture, 55, STAGE_HANDSHAKE);
  request_t request;
  start_request(&request, &fixture, 55, 20u);
  uint8_t* root = prepare_large_root(&request, large_root_length(55));
  launch_request(&request);
  wait_entered(&fixture, 55);
  set_time(&fixture, 20u);
  finish_request(&request, H2_PAL_ERR_TIMEOUT);
  memset(root, 0xff, request.config.root_ca_pem_len);
  free(root);
  memset(&request.config, 0xa5, sizeof(request.config));
  request.output = 987;
  assert(h2_bk_tls_tcp_close(&fixture.manager, 55) == H2_PAL_OK);
  assert(h2_bk_tls_tcp_register(&fixture.manager, 55) ==
         H2_PAL_ERR_INVALID_STATE);
  release_barrier(&fixture, 55);
  wait_closed(&fixture, 55, 1u);
  assert(fixture.plans[55].root_frees == 1u);
  assert(request.output == 987);
  fixture_destroy(&fixture);
}

static void test_blocked_dynamic_free(void) {
  fixture_t fixture;
  fixture_init(&fixture, 3u);
  open_fd(&fixture, 56, STAGE_ROOT_FREE);
  request_t request;
  start_request(&request, &fixture, 56, 20u);
  uint8_t* root = prepare_large_root(&request, large_root_length(56));
  launch_request(&request);
  finish_request(&request, H2_PAL_OK);
  free(root);
  wait_entered(&fixture, 56);
  set_time(&fixture, 1000u);
  assert(!h2_bk_tls_bio_canceled(&fixture.manager, 56));
  h2_bk_tls_tcp_ref_t ref;
  bool tls;
  assert(h2_bk_tls_tcp_acquire(&fixture.manager, 56, &ref, &tls) == H2_PAL_OK &&
         tls);
  assert(h2_bk_tls_tcp_release(&fixture.manager, &ref) == H2_PAL_OK);
  assert(h2_bk_tls_tcp_close(&fixture.manager, 56) == H2_PAL_OK);
  assert(h2_bk_tls_tcp_register(&fixture.manager, 56) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(fixture.plans[56].closes == 0u && fixture.plans[56].root_frees == 0u);
  open_fd(&fixture, 57, STAGE_SETUP);
  request_t other;
  start_request(&other, &fixture, 57, 20u);
  launch_request(&other);
  wait_entered(&fixture, 57);
  set_time(&fixture, 1020u);
  finish_request(&other, H2_PAL_ERR_TIMEOUT);
  assert(h2_bk_tls_tcp_close(&fixture.manager, 57) == H2_PAL_OK);
  release_barrier(&fixture, 57);
  wait_closed(&fixture, 57, 1u);
  release_barrier(&fixture, 56);
  wait_closed(&fixture, 56, 1u);
  assert(fixture.plans[56].root_frees == 1u);
  fixture_destroy(&fixture);
}

static void test_queued_large_allocation_cancel(void) {
  fixture_t fixture;
  fixture_init(&fixture, 0u);
  h2_bk_tls_lifetime_set_worker_ready(&fixture.manager, true);
  open_fd(&fixture, 61, STAGE_NONE);
  request_t request;
  start_request(&request, &fixture, 61, 20u);
  uint8_t* root = prepare_large_root(&request, large_root_length(61));
  launch_request(&request);
  wait_job_state(&fixture, 61, H2_BK_TLS_JOB_ALLOC_QUEUED);
  set_time(&fixture, 20u);
  finish_request(&request, H2_PAL_ERR_TIMEOUT);
  free(root);
  assert_io_denied(&fixture, 61);
  assert(h2_bk_tls_tcp_close(&fixture.manager, 61) == H2_PAL_OK);
  while (h2_bk_tls_worker_step(&fixture.manager)) {
  }
  assert(fixture.plans[61].root_allocs == 0u);
  assert(fixture.plans[61].root_frees == 0u);
  assert(fixture.plans[61].wraps == 0u);
  assert(fixture.plans[61].closes == 1u);
  fixture_destroy(&fixture);
}

int main(void) {
  test_queued_large_allocation_cancel();
  test_large_root_success();
  test_large_allocation_cancel(false);
  test_large_allocation_cancel(true);
  test_large_allocation_failure_and_missing_ops();
  test_large_copy_cancel(false);
  test_large_copy_cancel(true);
  test_large_input_after_wrap_timeout();
  test_blocked_dynamic_free();
  test_copy_budget_and_native_error();
  test_worker_deadline_wait();
  test_caller_claim_expiry_and_running_close();
  test_validation_and_registry_capacity();
  test_blocked_wrap(STAGE_SETUP);
  test_blocked_wrap(STAGE_HANDSHAKE);
  test_blocked_wrap(STAGE_PUBLICATION);
  test_deadline_boundary(9u, H2_PAL_OK);
  test_deadline_boundary(10u, H2_PAL_ERR_TIMEOUT);
  test_deadline_boundary(11u, H2_PAL_ERR_TIMEOUT);
  test_parallel_success_and_active_close();
  test_blocked_reaper(STAGE_DISPOSE);
  test_blocked_reaper(STAGE_CLOSE);
  test_queued_cancel_and_capacity();
  test_legacy_and_plain_refs();
  puts("BK TLS lifetime coordinator regressions passed");
  return 0;
}
