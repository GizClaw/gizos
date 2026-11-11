#include "h2_windows_tls_timeout_hooks.h"
#include "h2_wolfssl.h"

#include <assert.h>
#include <stdio.h>

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
static h2_pal_result_t clock_result;

h2_pal_result_t h2_test_windows_monotonic_ms(h2_windows_platform_t* platform,
                                             uint64_t* out_ms) {
  assert(platform != NULL && out_ms != NULL);
  *out_ms = now_ms;
  return clock_result;
}

int h2_test_tls_load_ca(WOLFSSL_CTX* context, const unsigned char* data,
                        long len, int format) {
  assert(context != NULL && data != NULL && len > 0);
  assert(format == WOLFSSL_FILETYPE_PEM);
  ++ca_loads;
  now_ms += setup_ms;
  return ca_result;
}

int h2_test_tls_load_system_ca(WOLFSSL_CTX* context) {
  assert(context != NULL);
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

int WSAAPI h2_test_tls_select(int nfds, fd_set* read_fds, fd_set* write_fds,
                              fd_set* error_fds,
                              const struct timeval* timeout) {
  assert(nfds == 0 && error_fds == NULL && timeout != NULL);
  assert((read_fds != NULL) == (first_error == WOLFSSL_ERROR_WANT_READ));
  assert((write_fds != NULL) == (first_error == WOLFSSL_ERROR_WANT_WRITE));
  ++waits;
  wait_budget_ms =
      (uint32_t)timeout->tv_sec * 1000u + (uint32_t)timeout->tv_usec / 1000u;
  now_ms += wait_ready ? ready_ms : wait_budget_ms;
  return wait_ready ? 1 : 0;
}

static h2_pal_result_t test_tls_wrap(void* user, h2_pal_net_socket_t token,
                                     const h2_pal_net_tls_config_t* config,
                                     uint32_t timeout_ms,
                                     h2_pal_net_socket_t* out_token) {
  return h2_test_windows_tls_wrap(user, token, config, timeout_ms, out_token);
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

static void run_case(h2_windows_platform_t* platform,
                     const timeout_case_t* test) {
  h2_pal_net_api_t net = *h2_windows_net_api(platform);
  h2_pal_net_vtable_t vtable = *net.vtable;
  vtable.tls_wrap = test_tls_wrap;
  net.vtable = &vtable;
  h2_pal_net_socket_t token = -1;
  assert(h2_pal_net_tcp_open_bound(&net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                   &token) == H2_PAL_OK);
  LONG baseline = platform->live_allocations;
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
      h2_pal_net_tls_wrap(&net, token, &config, test->timeout_ms, &out);
  if (result != test->expected) {
    fprintf(stderr, "%s: got %d, expected %d\n", test->name, result,
            test->expected);
  }
  assert(result == test->expected);
  assert(ca_loads == 1u);
  assert(connects == test->connects && waits == test->waits);
  assert(wait_budget_ms == test->wait_budget_ms);
  assert(out == (result == H2_PAL_OK ? token : -1));
  h2_windows_socket_slot_t* slot = h2_windows_net_lock_slot(platform, token);
  assert(slot != NULL);
  int socket_type = 0;
  int len = sizeof(socket_type);
  assert(getsockopt(slot->socket, SOL_SOCKET, SO_TYPE, (char*)&socket_type,
                    &len) == 0);
  assert(socket_type == SOCK_STREAM);
  if (result != H2_PAL_OK) {
    assert(slot->tls == NULL && slot->tls_context == NULL);
    assert(platform->live_allocations == baseline);
  }
  h2_windows_net_unlock_slot(slot);
  if (result != H2_PAL_OK) {
    /* A failed wrap releases TLS state and leaves the same socket token owned.
     */
    setup_ms = connect_ms = 0u;
    first_error = ca_result = WOLFSSL_SUCCESS;
    connects = 0u;
    assert(h2_pal_net_tls_wrap(&net, token, &config, 100u, &out) == H2_PAL_OK);
    assert(out == token);
  }
  h2_pal_net_close(&net, token);
  assert(platform->live_allocations == baseline);
}

int main(void) {
  const h2_windows_platform_config_t platform_config = {0};
  h2_windows_platform_t* platform = NULL;
  assert(h2_windows_platform_create(&platform_config, &platform) == H2_PAL_OK);
  const h2_wolfssl_config_t config = {
      .mem = *h2_windows_mem_api(platform),
      .entropy_user = platform,
      .entropy = h2_windows_entropy,
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
    run_case(platform, &cases[i]);
  }
  const h2_pal_net_api_t* net = h2_windows_net_api(platform);
  h2_pal_net_socket_t token = -1;
  assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL, &token) ==
         H2_PAL_OK);
  const h2_pal_net_tls_config_t tls = {0};
  h2_pal_net_socket_t out = 123;
  clock_result = H2_PAL_ERR_IO;
  assert(h2_test_windows_tls_wrap(platform, token, &tls, 100u, &out) ==
         H2_PAL_ERR_IO);
  assert(out == -1);
  clock_result = H2_PAL_OK;
  now_ms = UINT64_MAX - 10u;
  out = 123;
  assert(h2_test_windows_tls_wrap(platform, token, &tls, 100u, &out) ==
         H2_PAL_ERR_IO);
  assert(out == -1);
  h2_pal_net_close(net, token);
  assert(h2_wolfssl_deinit() == H2_PAL_OK);
  assert(h2_windows_platform_destroy(&platform) == H2_PAL_OK);
  return 0;
}
