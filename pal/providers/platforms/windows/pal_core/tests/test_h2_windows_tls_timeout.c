#include "h2_windows_tls_timeout_hooks.h"
#include "h2_wolfssl.h"
#include "fake_net.h"
#undef h2_windows_monotonic_ms
#undef CreateThread

#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

static atomic_uint_fast64_t now_ms;
static atomic_int blocking;
static atomic_int fail_thread;
static atomic_uint workers_started;
static SRWLOCK gate_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE gate = CONDITION_VARIABLE_INIT;
static unsigned entered;
static int release_loaders;
static h2_wolfssl_config_t backend;
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
static h2_pal_result_t clock_result;

h2_pal_result_t h2_test_windows_monotonic_ms(h2_windows_platform_t* platform,
                                             uint64_t* out_ms) {
  assert(platform != NULL && out_ms != NULL);
  if (blocking) return h2_windows_monotonic_ms(platform, out_ms);
  *out_ms = now_ms;
  return clock_result;
}

HANDLE WINAPI h2_test_tls_create_thread(LPSECURITY_ATTRIBUTES attributes,
                                        SIZE_T stack,
                                        LPTHREAD_START_ROUTINE entry,
                                        void* argument, DWORD flags,
                                        DWORD* id) {
  ++workers_started;
  if (atomic_exchange(&fail_thread, 0)) return NULL;
  return CreateThread(attributes, stack, entry, argument, flags, id);
}

static void load_gate(const unsigned char* data, long len) {
  if (!blocking) return;
  AcquireSRWLockExclusive(&gate_lock);
  ++entered;
  WakeAllConditionVariable(&gate);
  while (!release_loaders) {
    assert(SleepConditionVariableSRW(&gate, &gate_lock, INFINITE, 0));
  }
  ReleaseSRWLockExclusive(&gate_lock);
  if (data != NULL) assert(len == 9 && memcmp(data, "owned-ca", 9u) == 0);
}

int h2_test_tls_load_ca(WOLFSSL_CTX* context, const unsigned char* data,
                        long len, int format) {
  assert(context != NULL && data != NULL && len > 0);
  assert(format == WOLFSSL_FILETYPE_PEM);
  ++ca_loads;
  load_gate(data, len);
  now_ms += setup_ms;
  return ca_result;
}

int h2_test_tls_load_system_ca(WOLFSSL_CTX* context) {
  assert(context != NULL);
  ++ca_loads;
  load_gate(NULL, 0);
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

static h2_pal_result_t test_tls_wrap_bounded(
    void* user, h2_pal_net_socket_t token,
    const h2_pal_net_tls_config_t* config, uint32_t timeout_ms,
    h2_pal_net_socket_t* out_token) {
  return h2_test_windows_tls_wrap_bounded(user, token, config, timeout_ms,
                                          out_token);
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

static void drain_backend(void) {
  ULONGLONG deadline = GetTickCount64() + 5000u;
  h2_pal_result_t result;
  while ((result = h2_wolfssl_deinit()) == H2_PAL_ERR_INVALID_STATE) {
    assert(GetTickCount64() < deadline);
    Sleep(1u);
  }
  assert(result == H2_PAL_OK);
}

static void wait_for_init(const h2_wolfssl_config_t* config) {
  ULONGLONG deadline = GetTickCount64() + 5000u;
  h2_pal_result_t result;
  while ((result = h2_wolfssl_init(config)) == H2_PAL_ERR_INVALID_STATE) {
    assert(GetTickCount64() < deadline);
    Sleep(1u);
  }
  assert(result == H2_PAL_OK);
}

static void wait_platform_idle(h2_windows_platform_t* platform) {
  ULONGLONG deadline = GetTickCount64() + 5000u;
  while (InterlockedCompareExchange(&platform->live_objects, 0, 0) != 0) {
    assert(GetTickCount64() < deadline);
    Sleep(1u);
  }
}

static void run_case(h2_windows_platform_t* platform,
                     const timeout_case_t* test) {
  h2_pal_net_api_t net = *h2_windows_net_api(platform);
  h2_pal_net_vtable_t vtable = *net.vtable;
  vtable.tls_wrap = test_tls_wrap;
  vtable.tls_wrap_bounded = test_tls_wrap_bounded;
  net.vtable = &vtable;
  h2_pal_net_socket_t token = -1;
  assert(h2_pal_net_tcp_open_bound(&net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                   &token) == H2_PAL_OK);
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
      h2_pal_net_tls_wrap_bounded(&net, token, &config, test->timeout_ms, &out);
  if (result != test->expected) {
    fprintf(stderr, "%s: got %d, expected %d\n", test->name, result,
            test->expected);
  }
  assert(result == test->expected);
  assert(ca_loads == (test->timeout_ms == 0u ? 0u : 1u));
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
  }
  h2_windows_net_unlock_slot(slot);
  if (result != H2_PAL_OK) {
    /* A failed wrap releases TLS state and leaves the same socket token owned.
     */
    drain_backend();
    assert(platform->live_allocations == 0);
    assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
    setup_ms = connect_ms = 0u;
    first_error = ca_result = WOLFSSL_SUCCESS;
    connects = 0u;
    assert(h2_pal_net_tls_wrap_bounded(&net, token, &config, 100u, &out) ==
           H2_PAL_OK);
    assert(out == token);
  }
  h2_pal_net_close(&net, token);
  drain_backend();
  assert(platform->live_allocations == 0);
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
}

static void test_blocked_loads(h2_windows_platform_t* platform, int deferred) {
  h2_pal_net_api_t net = *h2_windows_net_api(platform);
  h2_pal_net_vtable_t vtable = *net.vtable;
  vtable.tls_wrap = test_tls_wrap;
  vtable.tls_wrap_bounded = test_tls_wrap_bounded;
  net.vtable = &vtable;
  setup_ms = connect_ms = 0u;
  first_error = ca_result = WOLFSSL_SUCCESS;
  connects = waits = 0u;
  blocking = 1;
  entered = 0u;
  release_loaders = 0;
  for (unsigned i = 0u; i < 8u; ++i) {
    void* page =
        VirtualAlloc(NULL, 4096u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(page != NULL);
    h2_pal_net_tls_config_t* config = page;
    char* name = (char*)(config + 1);
    memcpy(name, "localhost", sizeof("localhost"));
    char* alpn = name + 32;
    memcpy(alpn, "http/1.1", sizeof("http/1.1"));
    uint8_t* ca = (uint8_t*)alpn + 32;
    memcpy(ca, "owned-ca", 9u);
    *config = (h2_pal_net_tls_config_t){
        .server_name = name,
        .alpn = alpn,
        .root_ca_pem = i % 2u ? NULL : ca,
        .root_ca_pem_len = i % 2u ? 0u : 9u,
        .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
    };
    h2_pal_net_socket_t token = -1;
    assert(h2_pal_net_tcp_open_bound(&net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                     &token) == H2_PAL_OK);
    h2_pal_net_socket_t out = 123;
    ULONGLONG before = GetTickCount64();
    assert(h2_pal_net_tls_wrap_bounded(&net, token, config, 20u, &out) ==
           H2_PAL_ERR_TIMEOUT);
    assert(GetTickCount64() - before < 2000u && out == -1);
    h2_windows_socket_slot_t* slot = h2_windows_net_lock_slot(platform, token);
    assert(slot != NULL && slot->tls == NULL && slot->tls_context == NULL);
    h2_windows_net_unlock_slot(slot);
    assert(VirtualFree(page, 0u, MEM_RELEASE));
    h2_pal_net_close(&net, token);
    assert(h2_wolfssl_deinit() == H2_PAL_ERR_INVALID_STATE);
    assert(InterlockedCompareExchange(&platform->live_objects, 0, 0) > 0);
    h2_windows_platform_t* retained = platform;
    assert(h2_windows_platform_destroy(&retained) == H2_PAL_ERR_INVALID_STATE);
    assert(retained == platform);
  }
  h2_pal_net_socket_t token = -1;
  assert(h2_pal_net_tcp_open_bound(&net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                   &token) == H2_PAL_OK);
  const h2_pal_net_tls_config_t config = {
      .server_name = "localhost",
      .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
  };
  h2_pal_net_socket_t out = 123;
  assert(h2_pal_net_tls_wrap_bounded(&net, token, &config, 100u, &out) ==
         H2_PAL_ERR_NO_SPACE);
  assert(out == -1);
  h2_pal_net_close(&net, token);
  AcquireSRWLockExclusive(&gate_lock);
  while (entered != 8u) {
    assert(SleepConditionVariableSRW(&gate, &gate_lock, 5000u, 0));
  }
  h2_wolfssl_config_t changed = backend;
  changed.providers_thread_safe = false;
  if (deferred) {
    h2_wolfssl_deinit_deferred();
    assert(h2_wolfssl_init(&changed) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_wolfssl_operation_acquire() == H2_PAL_ERR_INVALID_STATE);
    assert(h2_wolfssl_operation_acquire_concurrent() ==
           H2_PAL_ERR_INVALID_STATE);
    h2_windows_platform_t* retained = platform;
    assert(h2_windows_platform_destroy(&retained) == H2_PAL_ERR_INVALID_STATE);
    assert(retained == platform);
  }
  release_loaders = 1;
  WakeAllConditionVariable(&gate);
  ReleaseSRWLockExclusive(&gate_lock);
  if (deferred) {
    wait_for_init(&changed);
    assert(h2_wolfssl_deinit() == H2_PAL_OK);
  } else {
    drain_backend();
  }
  wait_platform_idle(platform);
  assert(platform->live_allocations == 0);
  assert(InterlockedCompareExchange(&platform->live_objects, 0, 0) == 0);
  assert(connects == 0u && waits == 0u);
  blocking = 0;
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
}

static void test_thread_failure(h2_windows_platform_t* platform) {
  h2_pal_net_socket_t token = -1;
  const h2_pal_net_api_t* net = h2_windows_net_api(platform);
  assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL, &token) ==
         H2_PAL_OK);
  const h2_pal_net_tls_config_t config = {
      .server_name = "localhost",
      .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
  };
  now_ms = 1000u;
  fail_thread = 1;
  h2_pal_net_socket_t out = 123;
  assert(h2_test_windows_tls_wrap_bounded(platform, token, &config, 100u,
                                          &out) == H2_PAL_ERR_IO);
  assert(out == -1);
  h2_pal_net_close(net, token);
  drain_backend();
  wait_platform_idle(platform);
  assert(platform->live_allocations == 0);
  assert(InterlockedCompareExchange(&platform->live_objects, 0, 0) == 0);
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
}

typedef struct serial_provider {
  DWORD owner;
  h2_windows_platform_t* platform;
  size_t live;
  unsigned calls;
  unsigned entropy_calls;
} serial_provider_t;

static void* serial_alloc(void* user, size_t len) {
  serial_provider_t* provider = user;
  assert(provider->owner == GetCurrentThreadId());
  ++provider->calls;
  void* result = malloc(len);
  if (result != NULL) ++provider->live;
  return result;
}

static void* serial_realloc(void* user, void* memory, size_t len) {
  serial_provider_t* provider = user;
  assert(provider->owner == GetCurrentThreadId());
  ++provider->calls;
  if (memory == NULL) return serial_alloc(user, len);
  if (len == 0u) {
    free(memory);
    --provider->live;
    return NULL;
  }
  return realloc(memory, len);
}

static void serial_free(void* user, void* memory) {
  serial_provider_t* provider = user;
  assert(provider->owner == GetCurrentThreadId());
  ++provider->calls;
  if (memory != NULL) {
    --provider->live;
    free(memory);
  }
}

static int serial_entropy(void* user, uint8_t* out, size_t len) {
  serial_provider_t* provider = user;
  assert(provider->owner == GetCurrentThreadId());
  ++provider->entropy_calls;
  return h2_windows_entropy(provider->platform, out, len);
}

static const h2_pal_net_vtable_t* http_fake_base;
static h2_windows_platform_t* http_native_platform;

static int http_native_open(void* user, h2_pal_net_family_t family,
                            h2_pal_net_socket_t* out) {
  int result = http_fake_base->tcp_open(user, family, out);
  if (result != H2_PAL_OK) return result;
  return h2_pal_net_tcp_open_bound(h2_windows_net_api(http_native_platform),
                                   family, NULL, out);
}

static void http_native_close(void* user, h2_pal_net_socket_t socket) {
  h2_pal_net_close(h2_windows_net_api(http_native_platform), socket);
  http_fake_base->close(user, socket);
}

static h2_pal_result_t http_native_bounded(
    void* user, h2_pal_net_socket_t socket,
    const h2_pal_net_tls_config_t* config, uint32_t timeout_ms,
    h2_pal_net_socket_t* out) {
  fake_http_platform_t* http = user;
  ++http->tls_bounded_count;
  http->tls_timeout_ms = timeout_ms;
  return h2_test_windows_tls_wrap_bounded(http_native_platform, socket, config,
                                          timeout_ms, out);
}

static h2_pal_result_t http_native_legacy(void* user,
                                          h2_pal_net_socket_t socket,
                                          const h2_pal_net_tls_config_t* config,
                                          uint32_t timeout_ms,
                                          h2_pal_net_socket_t* out) {
  fake_http_platform_t* http = user;
  ++http->tls_wrap_count;
  return h2_test_windows_tls_wrap(http_native_platform, socket, config,
                                  timeout_ms, out);
}

static void test_http_bounded_rejection(serial_provider_t* provider,
                                        h2_windows_platform_t* platform) {
  fake_http_platform_t http;
  fake_http_platform_init(&http);
  fake_http_platform_add_response(
      &http, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
  http_fake_base = http.net.vtable;
  http_native_platform = platform;
  h2_pal_net_vtable_t vtable = *http_fake_base;
  vtable.tcp_open = http_native_open;
  vtable.close = http_native_close;
  vtable.tls_wrap = http_native_legacy;
  vtable.tls_wrap_bounded = http_native_bounded;
  http.net.vtable = &vtable;
  const h2_corehttp_config_t config = {
      .allocator = &http.mem,
      .net = &http.net,
      .time = &http.time,
      .default_timeout_ms = 100u,
      .default_connect_timeout_ms = 50u,
  };
  h2_corehttp_t* client = NULL;
  h2_pal_http_api_t api;
  assert(h2_corehttp_create(&config, &client, &api) == H2_PAL_OK);
  const char url[] = "https://legacy.test/bounded";
  const h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_GET,
      .url = {.data = url, .len = sizeof(url) - 1u},
      .timeout_ms = 100,
      .connect_timeout_ms = 50,
  };
  now_ms = 1000u;
  unsigned callbacks = provider->calls;
  unsigned entropy_callbacks = provider->entropy_calls;
  unsigned started = workers_started;
  unsigned loaded = ca_loads;
  ULONGLONG before = GetTickCount64();
  h2_pal_http_response_t response;
  assert(h2_pal_http_request(&api, &request, &response) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(GetTickCount64() - before < 2000u);
  assert(response.rejection == H2_PAL_HTTP_REJECTION_NONE);
  assert(provider->calls == callbacks &&
         provider->entropy_calls == entropy_callbacks);
  assert(workers_started == started && ca_loads == loaded);
  assert(http.tls_bounded_count == 1 && http.tls_wrap_count == 0);
  assert(http.tls_timeout_ms > 0u && http.tls_timeout_ms <= 50u);
  assert(http.connect_count == 1 && http.close_count == 1);
  assert(http.request_len == 0u && http.recv_count == 0);
  h2_corehttp_destroy(client);
  http_fake_base = NULL;
}

static void test_legacy_inline_setup(h2_windows_platform_t* platform) {
  drain_backend();
  wait_platform_idle(platform);
  serial_provider_t provider = {
      .owner = GetCurrentThreadId(),
      .platform = platform,
  };
  const h2_pal_mem_vtable_t vtable = {
      .alloc = serial_alloc,
      .realloc = serial_realloc,
      .free = serial_free,
  };
  const h2_wolfssl_config_t config = {
      .mem = {.user = &provider, .vtable = &vtable},
      .entropy_user = &provider,
      .entropy = serial_entropy,
  };
  assert(!config.providers_thread_safe);
  assert(h2_wolfssl_init(&config) == H2_PAL_OK);
  assert(h2_wolfssl_operation_acquire_concurrent() == H2_PAL_ERR_UNSUPPORTED);
  test_http_bounded_rejection(&provider, platform);
  const h2_pal_net_api_t* net = h2_windows_net_api(platform);
  const h2_pal_net_tls_config_t tls = {
      .server_name = "localhost",
      .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
  };
  unsigned started = workers_started;
  for (unsigned i = 0u; i < 32u; ++i) {
    h2_pal_net_socket_t token = -1;
    assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                     &token) == H2_PAL_OK);
    size_t baseline = provider.live;
    unsigned loaded = ca_loads;
    now_ms = 1000u;
    setup_ms = i % 4u == 0u ? 0u : i % 4u == 2u ? 100u : 60u;
    connect_ms = i % 4u == 1u ? 5u : i % 4u == 3u ? 40u : 0u;
    first_error = i % 4u == 1u ? WOLFSSL_ERROR_WANT_READ : WOLFSSL_SUCCESS;
    ca_result = WOLFSSL_SUCCESS;
    wait_ready = 0;
    connects = waits = 0u;
    wait_budget_ms = 0u;
    h2_pal_net_socket_t out = 123;
    h2_pal_result_t expected = i % 4u == 0u ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
    unsigned calls = provider.calls;
    unsigned entropy_calls = provider.entropy_calls;
    ULONGLONG before = GetTickCount64();
    assert(h2_test_windows_tls_wrap_bounded(platform, token, &tls, 100u,
                                            &out) == H2_PAL_ERR_UNSUPPORTED);
    assert(GetTickCount64() - before < 2000u && out == -1);
    assert(provider.calls == calls && provider.entropy_calls == entropy_calls);
    assert(workers_started == started && ca_loads == loaded);
    assert(h2_test_windows_tls_wrap(platform, token, &tls, 100u, &out) ==
           expected);
    assert(out == (expected == H2_PAL_OK ? token : -1));
    assert(workers_started == started && ca_loads == loaded + 1u);
    h2_windows_socket_slot_t* slot = h2_windows_net_lock_slot(platform, token);
    assert(slot != NULL);
    if (expected != H2_PAL_OK)
      assert(slot->tls == NULL && slot->tls_context == NULL);
    h2_windows_net_unlock_slot(slot);
    if (i % 4u == 1u) assert(wait_budget_ms == 35u);
    if (i % 4u == 2u) assert(connects == 0u);
    h2_pal_net_close(net, token);
    assert(provider.live == baseline);
    void* memory = h2_pal_mem_alloc(&config.mem, 16u);
    assert(memory != NULL);
    memory = h2_pal_mem_realloc(&config.mem, memory, 32u);
    assert(memory != NULL);
    h2_pal_mem_free(&config.mem, memory);
    uint8_t bytes[8];
    assert(serial_entropy(&provider, bytes, sizeof(bytes)) == H2_PAL_OK);
  }
  assert(h2_wolfssl_deinit() == H2_PAL_OK);
  assert(provider.live == 0u);
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
}

static void test_resume_blocked_load(h2_windows_platform_t* platform) {
  const h2_pal_net_api_t* net = h2_windows_net_api(platform);
  setup_ms = connect_ms = 0u;
  first_error = ca_result = WOLFSSL_SUCCESS;
  connects = waits = 0u;
  blocking = 1;
  entered = 0u;
  release_loaders = 0;
  h2_pal_net_socket_t token = -1;
  assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL, &token) ==
         H2_PAL_OK);
  const h2_pal_net_tls_config_t old = {
      .server_name = "localhost",
      .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
  };
  h2_pal_net_socket_t out = -1;
  assert(h2_test_windows_tls_wrap_bounded(platform, token, &old, 20u, &out) ==
         H2_PAL_ERR_TIMEOUT);
  h2_pal_net_close(net, token);
  AcquireSRWLockExclusive(&gate_lock);
  while (entered != 1u) {
    assert(SleepConditionVariableSRW(&gate, &gate_lock, 5000u, 0));
  }
  ReleaseSRWLockExclusive(&gate_lock);
  h2_wolfssl_deinit_deferred();
  h2_wolfssl_config_t changed = backend;
  changed.providers_thread_safe = false;
  assert(h2_wolfssl_init(&changed) == H2_PAL_ERR_INVALID_STATE);
  LONG retained = InterlockedCompareExchange(&platform->live_allocations, 0, 0);
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
  assert(InterlockedCompareExchange(&platform->live_allocations, 0, 0) ==
         retained);
  assert(h2_wolfssl_operation_acquire_concurrent() == H2_PAL_OK);
  h2_wolfssl_operation_release();
  uint8_t random[8];
  assert(h2_pal_crypto_random(h2_wolfssl_crypto_api(), random,
                              sizeof(random)) == H2_PAL_OK);
  const h2_pal_net_tls_config_t fresh = {
      .server_name = "localhost",
      .verify = H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY,
  };
  assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL, &token) ==
         H2_PAL_OK);
  assert(h2_test_windows_tls_wrap_bounded(platform, token, &fresh, 1000u,
                                          &out) == H2_PAL_OK);
  assert(out == token);
  h2_pal_net_close(net, token);
  assert(h2_wolfssl_deinit() == H2_PAL_ERR_INVALID_STATE);
  h2_wolfssl_deinit_deferred();
  assert(h2_wolfssl_init(&changed) == H2_PAL_ERR_INVALID_STATE);
  AcquireSRWLockExclusive(&gate_lock);
  release_loaders = 1;
  WakeAllConditionVariable(&gate);
  ReleaseSRWLockExclusive(&gate_lock);
  wait_for_init(&changed);
  assert(h2_wolfssl_deinit() == H2_PAL_OK);
  wait_platform_idle(platform);
  assert(platform->live_allocations == 0);
  blocking = 0;
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
}

static DWORD WINAPI race_final_release(void* event) {
  assert(WaitForSingleObject(event, 5000u) == WAIT_OBJECT_0);
  h2_wolfssl_operation_release();
  return 0u;
}

static void test_deferred_release_vs_reinit(void) {
  h2_wolfssl_config_t changed = backend;
  changed.providers_thread_safe = false;
  for (unsigned i = 0u; i < 100u; ++i) {
    assert(h2_wolfssl_operation_acquire_concurrent() == H2_PAL_OK);
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(event != NULL);
    HANDLE worker = CreateThread(NULL, 0u, race_final_release, event, 0u, NULL);
    assert(worker != NULL);
    assert(SetEvent(event));
    h2_wolfssl_deinit_deferred();
    const h2_wolfssl_config_t* wanted = i % 2u == 0u ? &backend : &changed;
    wait_for_init(wanted);
    assert(WaitForSingleObject(worker, 5000u) == WAIT_OBJECT_0);
    assert(CloseHandle(worker));
    assert(CloseHandle(event));
    if (wanted == &backend) {
      assert(h2_wolfssl_operation_acquire_concurrent() == H2_PAL_OK);
      h2_wolfssl_operation_release();
      assert(h2_wolfssl_init(&changed) == H2_PAL_ERR_INVALID_STATE);
    } else {
      assert(h2_wolfssl_operation_acquire_concurrent() ==
             H2_PAL_ERR_UNSUPPORTED);
      assert(h2_wolfssl_init(&backend) == H2_PAL_ERR_INVALID_STATE);
    }
    assert(h2_wolfssl_deinit() == H2_PAL_OK);
    assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
  }
}

int main(void) {
  const h2_windows_platform_config_t platform_config = {0};
  h2_windows_platform_t* platform = NULL;
  assert(h2_windows_platform_create(&platform_config, &platform) == H2_PAL_OK);
  backend = (h2_wolfssl_config_t){
      .mem = *h2_windows_mem_api(platform),
      .entropy_user = platform,
      .entropy = h2_windows_entropy,
      /* Windows Heap, interlocked accounting, and BCrypt entropy are
         concurrent. */
      .providers_thread_safe = true,
  };
  assert(h2_wolfssl_init(&backend) == H2_PAL_OK);
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
      {"CA format error", 90, 100, 0, WOLFSSL_SUCCESS, 0, 0, 0, WOLFSSL_FAILURE,
       H2_PAL_ERR_FORMAT, 0, 0, 0},
      {"system CA I/O error", 90, 100, 0, WOLFSSL_SUCCESS, 0, 0, 1,
       WOLFSSL_FAILURE, H2_PAL_ERR_IO, 0, 0, 0},
  };
  for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    run_case(platform, &cases[i]);
  }
  test_thread_failure(platform);
  test_legacy_inline_setup(platform);
  test_blocked_loads(platform, 0);
  test_blocked_loads(platform, 1);
  test_resume_blocked_load(platform);
  test_deferred_release_vs_reinit();
  const h2_pal_net_api_t* net = h2_windows_net_api(platform);
  h2_pal_net_socket_t token = -1;
  assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL, &token) ==
         H2_PAL_OK);
  const h2_pal_net_tls_config_t tls = {0};
  h2_pal_net_socket_t out = 123;
  clock_result = H2_PAL_ERR_IO;
  assert(h2_test_windows_tls_wrap_bounded(platform, token, &tls, 100u, &out) ==
         H2_PAL_ERR_IO);
  assert(out == -1);
  clock_result = H2_PAL_OK;
  now_ms = UINT64_MAX - 10u;
  out = 123;
  assert(h2_test_windows_tls_wrap_bounded(platform, token, &tls, 100u, &out) ==
         H2_PAL_ERR_IO);
  assert(out == -1);
  h2_pal_net_close(net, token);
  assert(h2_wolfssl_deinit() == H2_PAL_OK);
  wait_platform_idle(platform);
  assert(h2_windows_platform_destroy(&platform) == H2_PAL_OK);
  return 0;
}
