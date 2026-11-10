/* The target substitutes only the clock, certificate loading, and handshake
 * I/O. Socket ownership, TLS allocation/setup, and cleanup use the real
 * provider. */
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
#include <stdlib.h>
#include <sys/select.h>
#include <sys/time.h>

#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>

static uint64_t now_ms;
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
static unsigned ca_loads;
static size_t allocations;

int h2_test_gettimeofday(struct timeval* now, void* timezone) {
  (void)timezone;
  now->tv_sec = (time_t)(now_ms / 1000u);
  now->tv_usec = (suseconds_t)(now_ms % 1000u) * 1000;
  return 0;
}

int h2_test_tls_load_ca(WOLFSSL_CTX* ctx, const unsigned char* data, long len,
                        int format) {
  assert(ctx != NULL && data != NULL && len > 0);
  assert(format == WOLFSSL_FILETYPE_PEM);
  ++ca_loads;
  now_ms += setup_ms;
  return ca_result;
}

int h2_test_tls_load_system_ca(WOLFSSL_CTX* ctx) {
  assert(ctx != NULL);
  ++ca_loads;
  now_ms += setup_ms;
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

static void run_case(const timeout_case_t* test) {
  const h2_pal_net_api_t* net = h2_posix_net_api();
  h2_pal_net_socket_t socket_fd = -1;
  assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                   &socket_fd) == H2_PAL_OK);
  int flags = fcntl(socket_fd, F_GETFL, 0);
  assert(flags >= 0);
  size_t baseline = allocations;
  now_ms = 1000u;
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
  assert(ca_loads == 1u);
  assert(connects == test->connects && waits == test->waits);
  assert(wait_budget_ms == test->wait_budget_ms);
  assert(out == (result == H2_PAL_OK ? socket_fd : -1));
  assert(fcntl(socket_fd, F_GETFL, 0) >= 0);
  if (result != H2_PAL_OK) {
    assert(fcntl(socket_fd, F_GETFL, 0) == flags);
    assert(allocations == baseline);
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
  assert(allocations == baseline);
}

int main(void) {
  const h2_pal_mem_vtable_t mem = {
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  const h2_wolfssl_config_t config = {
      .mem = {.vtable = &mem},
      .entropy = h2_posix_entropy,
  };
  assert(h2_wolfssl_init(&config) == H2_PAL_OK);
  const timeout_case_t cases[] = {
      {"CA reduces read wait", 60, 100, 5, WOLFSSL_ERROR_WANT_READ, 0, 0, 0,
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
      {"CA format error", 101, 100, 0, WOLFSSL_SUCCESS, 0, 0, 0,
       WOLFSSL_FAILURE, H2_PAL_ERR_FORMAT, 0, 0, 0},
      {"system CA I/O error", 101, 100, 0, WOLFSSL_SUCCESS, 0, 0, 1,
       WOLFSSL_FAILURE, H2_PAL_ERR_IO, 0, 0, 0},
  };
  for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    run_case(&cases[i]);
  }
  assert(h2_wolfssl_deinit() == H2_PAL_OK);
  assert(allocations == 0u);
  return 0;
}
