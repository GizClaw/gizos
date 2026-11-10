/* The target substitutes only the clock, certificate loading, and handshake
 * I/O. Socket ownership, TLS allocation/setup, and cleanup use the real
 * provider. */
#undef clock_gettime
#undef pthread_create
#undef gettimeofday
#undef select
#undef wolfSSL_CTX_load_verify_buffer
#undef wolfSSL_CTX_load_system_CA_certs
#undef wolfSSL_connect
#undef wolfSSL_get_error

#include "h2_posix_pal_core.h"
#include "h2_wolfssl.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/time.h>

#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>

static atomic_uint_fast64_t now_ms;
static atomic_uint_fast64_t wall_ms;
static int backward_wall;
static atomic_int blocking;
static atomic_int fail_thread;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate = PTHREAD_COND_INITIALIZER;
static unsigned entered;
static int release_loaders;
static uint32_t setup_ms;
static uint32_t connect_ms;
static uint32_t ready_ms;
static uint32_t wait_budget_ms;
static int ca_result;
static int first_error;
static int last_error;
static int wait_ready;
static unsigned connects;
static unsigned waits;
static atomic_uint ca_loads;
static atomic_size_t allocations;

int h2_test_gettimeofday(struct timeval* now, void* timezone) {
  (void)timezone;
  now->tv_sec = (time_t)(wall_ms / 1000u);
  now->tv_usec = (suseconds_t)(wall_ms % 1000u) * 1000;
  return 0;
}

int h2_test_clock_gettime(clockid_t clock, struct timespec* now) {
  assert(clock == CLOCK_MONOTONIC);
  if (blocking) return clock_gettime(clock, now);
  uint64_t ms = now_ms;
  now->tv_sec = (time_t)(ms / 1000u);
  now->tv_nsec = (long)(ms % 1000u) * 1000000L;
  return 0;
}

int h2_test_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                           void* (*entry)(void*), void* arg) {
  if (atomic_exchange(&fail_thread, 0)) return EAGAIN;
  return pthread_create(thread, attr, entry, arg);
}

static void load_gate(const unsigned char* data, long len) {
  if (!blocking) return;
  assert(pthread_mutex_lock(&gate_lock) == 0);
  ++entered;
  pthread_cond_broadcast(&gate);
  while (!release_loaders) pthread_cond_wait(&gate, &gate_lock);
  assert(pthread_mutex_unlock(&gate_lock) == 0);
  /* The caller has unmapped all its inputs before this read. */
  if (data != NULL) assert(len == 9 && memcmp(data, "owned-ca", 9u) == 0);
}

int h2_test_tls_load_ca(WOLFSSL_CTX* ctx, const unsigned char* data, long len,
                        int format) {
  assert(ctx != NULL && data != NULL && len > 0);
  assert(format == WOLFSSL_FILETYPE_PEM);
  ++ca_loads;
  load_gate(data, len);
  now_ms += setup_ms;
  if (backward_wall) wall_ms = 1u;
  return ca_result;
}

int h2_test_tls_load_system_ca(WOLFSSL_CTX* ctx) {
  assert(ctx != NULL);
  ++ca_loads;
  load_gate(NULL, 0);
  now_ms += setup_ms;
  if (backward_wall) wall_ms = 1u;
  return ca_result;
}

int h2_test_tls_connect(WOLFSSL* ssl) {
  assert(ssl != NULL);
  now_ms += connect_ms;
  last_error = connects++ == 0u ? first_error : WOLFSSL_SUCCESS;
  return last_error == WOLFSSL_SUCCESS ? WOLFSSL_SUCCESS : WOLFSSL_FAILURE;
}

int h2_test_tls_get_error(WOLFSSL* ssl, int result) {
  assert(ssl != NULL && result == WOLFSSL_FAILURE);
  return last_error;
}

int h2_test_tls_select(int nfds, fd_set* read_fds, fd_set* write_fds,
                       fd_set* error_fds, struct timeval* timeout) {
  assert(nfds > 0 && error_fds == NULL && timeout != NULL);
  assert((read_fds != NULL) == (first_error == WOLFSSL_ERROR_WANT_READ));
  assert((write_fds != NULL) == (first_error == WOLFSSL_ERROR_WANT_WRITE));
  ++waits;
  wait_budget_ms =
      (uint32_t)timeout->tv_sec * 1000u + (uint32_t)timeout->tv_usec / 1000u;
  now_ms += wait_ready ? ready_ms : wait_budget_ms;
  return wait_ready ? 1 : 0;
}

static void* test_alloc(void* user, size_t len) {
  (void)user;
  void* memory = malloc(len);
  if (memory != NULL) ++allocations;
  return memory;
}

static void* test_realloc(void* user, void* memory, size_t len) {
  (void)user;
  if (memory == NULL) return test_alloc(NULL, len);
  if (len == 0u) {
    free(memory);
    --allocations;
    return NULL;
  }
  return realloc(memory, len);
}

static void test_free(void* user, void* memory) {
  (void)user;
  if (memory != NULL) {
    assert(allocations > 0u);
    --allocations;
    free(memory);
  }
}

typedef struct timeout_case {
  const char* name;
  uint32_t setup_ms;
  uint32_t timeout_ms;
  uint32_t connect_ms;
  int first_error;
  int wait_ready;
  uint32_t ready_ms;
  int system_ca;
  int ca_result;
  h2_pal_result_t expected;
  unsigned connects;
  unsigned waits;
  uint32_t wait_budget_ms;
} timeout_case_t;

static const h2_pal_mem_vtable_t memory = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};
static const h2_wolfssl_config_t backend = {
    .mem = {.vtable = &memory},
    .entropy = h2_posix_entropy,
};

static uint64_t real_ms(void) {
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void drain_backend(void) {
  uint64_t deadline = real_ms() + 5000u;
  h2_pal_result_t result;
  while ((result = h2_wolfssl_deinit()) == H2_PAL_ERR_INVALID_STATE) {
    assert(real_ms() < deadline);
    const struct timespec tick = {.tv_nsec = 1000000};
    nanosleep(&tick, NULL);
  }
  assert(result == H2_PAL_OK);
  assert(allocations == 0u);
}

static void run_case(const timeout_case_t* test) {
  const h2_pal_net_api_t* net = h2_posix_net_api();
  h2_pal_net_socket_t socket_fd = -1;
  assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                   &socket_fd) == H2_PAL_OK);
  int flags = fcntl(socket_fd, F_GETFL, 0);
  assert(flags >= 0);
  now_ms = 1000u;
  wall_ms = 1000000u;
  backward_wall = strcmp(test->name, "backward wall jump") == 0;
  setup_ms = test->setup_ms;
  connect_ms = test->connect_ms;
  first_error = test->first_error;
  wait_ready = test->wait_ready;
  ready_ms = test->ready_ms;
  ca_result = test->ca_result;
  connects = waits = ca_loads = 0u;
  wait_budget_ms = 0u;
  static const uint8_t ca[] = "controlled certificate loader";
  const h2_pal_net_tls_config_t config = {
      .server_name = "localhost",
      .root_ca_pem = test->system_ca ? NULL : ca,
      .root_ca_pem_len = test->system_ca ? 0u : sizeof(ca),
      .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
  };
  h2_pal_net_socket_t out = 123;
  h2_pal_result_t result =
      h2_pal_net_tls_wrap(net, socket_fd, &config, test->timeout_ms, &out);
  if (result != test->expected) {
    fprintf(stderr, "%s: got %d, expected %d\n", test->name, result,
            test->expected);
  }
  assert(result == test->expected);
  assert(ca_loads == (test->timeout_ms == 0u ? 0u : 1u));
  assert(connects == test->connects && waits == test->waits);
  assert(wait_budget_ms == test->wait_budget_ms);
  assert(out == (result == H2_PAL_OK ? socket_fd : -1));
  assert(fcntl(socket_fd, F_GETFL, 0) >= 0);
  if (result != H2_PAL_OK) {
    assert(fcntl(socket_fd, F_GETFL, 0) == flags);
    drain_backend();
    assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
    /* Reuse the same TCP socket to prove timeout/error releases its TLS slot.
     */
    setup_ms = connect_ms = 0u;
    first_error = ca_result = WOLFSSL_SUCCESS;
    connects = 0u;
    assert(h2_pal_net_tls_wrap(net, socket_fd, &config, 100u, &out) ==
           H2_PAL_OK);
    assert(out == socket_fd);
  }
  h2_pal_net_close(net, socket_fd);
  drain_backend();
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
}

static void test_blocked_loads(void) {
  const h2_pal_net_api_t* net = h2_posix_net_api();
  setup_ms = connect_ms = 0u;
  backward_wall = 0;
  first_error = ca_result = WOLFSSL_SUCCESS;
  connects = waits = 0u;
  blocking = 1;
  entered = 0u;
  release_loaders = 0;
  long page_size = sysconf(_SC_PAGESIZE);
  assert(page_size > 0);
  for (unsigned i = 0u; i < 8u; ++i) {
    void* page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(page != MAP_FAILED);
    h2_pal_net_tls_config_t* config = page;
    char* name = (char*)(config + 1);
    strcpy(name, "localhost");
    char* alpn = name + 32;
    strcpy(alpn, "http/1.1");
    uint8_t* ca = (uint8_t*)alpn + 32;
    memcpy(ca, "owned-ca", 9u);
    *config = (h2_pal_net_tls_config_t){
        .server_name = name,
        .alpn = alpn,
        .root_ca_pem = ca,
        .root_ca_pem_len = 9u,
        .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
    };
    h2_pal_net_socket_t socket_fd = -1;
    assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                     &socket_fd) == H2_PAL_OK);
    int flags = fcntl(socket_fd, F_GETFL, 0);
    h2_pal_net_socket_t out = 123;
    uint64_t before = real_ms();
    assert(h2_pal_net_tls_wrap(net, socket_fd, config, 20u, &out) ==
           H2_PAL_ERR_TIMEOUT);
    assert(real_ms() - before < 2000u);
    assert(out == -1 && fcntl(socket_fd, F_GETFL, 0) == flags);
    /* Destroy the caller's config, CA bytes, SNI and ALPN while setup is
     * blocked. */
    assert(munmap(page, (size_t)page_size) == 0);
    h2_pal_net_close(net, socket_fd);
    assert(h2_wolfssl_deinit() == H2_PAL_ERR_INVALID_STATE);
  }
  h2_pal_net_socket_t socket_fd = -1;
  assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                   &socket_fd) == H2_PAL_OK);
  const h2_pal_net_tls_config_t config = {
      .server_name = "localhost",
      .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
  };
  h2_pal_net_socket_t out = 123;
  assert(h2_pal_net_tls_wrap(net, socket_fd, &config, 100u, &out) ==
         H2_PAL_ERR_NO_SPACE);
  assert(out == -1 && fcntl(socket_fd, F_GETFL, 0) >= 0);
  h2_pal_net_close(net, socket_fd);
  assert(pthread_mutex_lock(&gate_lock) == 0);
  while (entered != 8u) {
    struct timespec limit;
    assert(clock_gettime(CLOCK_REALTIME, &limit) == 0);
    limit.tv_sec += 5;
    assert(pthread_cond_timedwait(&gate, &gate_lock, &limit) == 0);
  }
  release_loaders = 1;
  pthread_cond_broadcast(&gate);
  assert(pthread_mutex_unlock(&gate_lock) == 0);
  drain_backend();
  assert(connects == 0u && waits == 0u);
  blocking = 0;
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
}

static void test_setup_admission_failures(void) {
  const h2_pal_net_api_t* net = h2_posix_net_api();
  h2_pal_net_socket_t socket_fd = -1;
  assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                   &socket_fd) == H2_PAL_OK);
  h2_pal_net_tls_config_t config = {
      .server_name = "localhost",
      .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
  };
  h2_pal_net_socket_t out = 123;
  now_ms = 1000u;
  fail_thread = 1;
  assert(h2_pal_net_tls_wrap(net, socket_fd, &config, 100u, &out) ==
         H2_PAL_ERR_IO);
  assert(out == -1 && fcntl(socket_fd, F_GETFL, 0) >= 0);
  drain_backend();
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
  size_t ca_len = 4u * 1024u * 1024u + 1u;
  uint8_t* ca = malloc(ca_len);
  assert(ca != NULL);
  config.root_ca_pem = ca;
  config.root_ca_pem_len = ca_len;
  out = 123;
  assert(h2_pal_net_tls_wrap(net, socket_fd, &config, 100u, &out) ==
         H2_PAL_ERR_NO_SPACE);
  assert(out == -1 && fcntl(socket_fd, F_GETFL, 0) >= 0);
  free(ca);
  h2_pal_net_close(net, socket_fd);
  drain_backend();
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
}

typedef struct lease_race {
  pthread_mutex_t lock;
  pthread_cond_t condition;
  int start;
  int release;
  h2_pal_result_t acquired;
} lease_race_t;

static void* race_acquire(void* argument) {
  lease_race_t* race = argument;
  pthread_mutex_lock(&race->lock);
  while (!race->start) pthread_cond_wait(&race->condition, &race->lock);
  pthread_mutex_unlock(&race->lock);
  race->acquired = h2_wolfssl_operation_acquire();
  pthread_mutex_lock(&race->lock);
  while (!race->release) pthread_cond_wait(&race->condition, &race->lock);
  pthread_mutex_unlock(&race->lock);
  if (race->acquired == H2_PAL_OK) h2_wolfssl_operation_release();
  return NULL;
}

static void test_acquire_vs_deinit(void) {
  for (unsigned i = 0u; i < 100u; ++i) {
    lease_race_t race = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
    };
    pthread_t worker;
    assert(pthread_create(&worker, NULL, race_acquire, &race) == 0);
    pthread_mutex_lock(&race.lock);
    race.start = 1;
    pthread_cond_broadcast(&race.condition);
    pthread_mutex_unlock(&race.lock);
    h2_pal_result_t result = h2_wolfssl_deinit();
    pthread_mutex_lock(&race.lock);
    race.release = 1;
    pthread_cond_broadcast(&race.condition);
    pthread_mutex_unlock(&race.lock);
    assert(pthread_join(worker, NULL) == 0);
    if (race.acquired == H2_PAL_OK) {
      assert(result == H2_PAL_ERR_INVALID_STATE);
      assert(h2_wolfssl_deinit() == H2_PAL_OK);
    } else {
      assert(race.acquired == H2_PAL_ERR_INVALID_STATE && result == H2_PAL_OK);
    }
    assert(h2_wolfssl_operation_acquire() == H2_PAL_ERR_INVALID_STATE);
    pthread_cond_destroy(&race.condition);
    pthread_mutex_destroy(&race.lock);
    assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
  }
}

int main(void) {
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
  const timeout_case_t cases[] = {
      {"CA reduces read wait", 60, 100, 5, WOLFSSL_ERROR_WANT_READ, 0, 0, 0,
       WOLFSSL_SUCCESS, H2_PAL_ERR_TIMEOUT, 1, 1, 35},
      {"backward wall jump", 60, 100, 5, WOLFSSL_ERROR_WANT_READ, 0, 0, 0,
       WOLFSSL_SUCCESS, H2_PAL_ERR_TIMEOUT, 1, 1, 35},
      {"system CA reduces wait", 60, 100, 5, WOLFSSL_ERROR_WANT_WRITE, 0, 0, 1,
       WOLFSSL_SUCCESS, H2_PAL_ERR_TIMEOUT, 1, 1, 35},
      {"setup exhausts budget", 100, 100, 0, WOLFSSL_SUCCESS, 0, 0, 0,
       WOLFSSL_SUCCESS, H2_PAL_ERR_TIMEOUT, 0, 0, 0},
      {"setup exceeds budget", 101, 100, 0, WOLFSSL_SUCCESS, 0, 0, 1,
       WOLFSSL_SUCCESS, H2_PAL_ERR_TIMEOUT, 0, 0, 0},
      {"late success", 60, 100, 41, WOLFSSL_SUCCESS, 0, 0, 0, WOLFSSL_SUCCESS,
       H2_PAL_ERR_TIMEOUT, 1, 0, 0},
      {"success at deadline", 60, 100, 40, WOLFSSL_SUCCESS, 0, 0, 0,
       WOLFSSL_SUCCESS, H2_PAL_ERR_TIMEOUT, 1, 0, 0},
      {"success before deadline", 60, 100, 39, WOLFSSL_SUCCESS, 0, 0, 0,
       WOLFSSL_SUCCESS, H2_PAL_OK, 1, 0, 0},
      {"readiness at deadline", 60, 100, 5, WOLFSSL_ERROR_WANT_READ, 1, 35, 0,
       WOLFSSL_SUCCESS, H2_PAL_ERR_TIMEOUT, 1, 1, 35},
      {"retry before deadline", 60, 100, 5, WOLFSSL_ERROR_WANT_WRITE, 1, 10, 0,
       WOLFSSL_SUCCESS, H2_PAL_OK, 2, 1, 35},
      {"zero budget", 0, 0, 0, WOLFSSL_SUCCESS, 0, 0, 0, WOLFSSL_SUCCESS,
       H2_PAL_ERR_TIMEOUT, 0, 0, 0},
      {"verification error", 60, 100, 50, DOMAIN_NAME_MISMATCH, 0, 0, 0,
       WOLFSSL_SUCCESS, H2_PAL_ERR_TLS_VERIFY, 1, 0, 0},
      {"handshake I/O error", 60, 100, 50, WOLFSSL_ERROR_SYSCALL, 0, 0, 0,
       WOLFSSL_SUCCESS, H2_PAL_ERR_IO, 1, 0, 0},
      {"CA format error", 90, 100, 0, WOLFSSL_SUCCESS, 0, 0, 0, WOLFSSL_FAILURE,
       H2_PAL_ERR_FORMAT, 0, 0, 0},
      {"system CA I/O error", 90, 100, 0, WOLFSSL_SUCCESS, 0, 0, 1,
       WOLFSSL_FAILURE, H2_PAL_ERR_IO, 0, 0, 0},
  };
  for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    run_case(&cases[i]);
  }
  test_setup_admission_failures();
  test_blocked_loads();
  test_acquire_vs_deinit();
  assert(h2_wolfssl_deinit() == H2_PAL_OK);
  assert(allocations == 0u);
  return 0;
}
