#include "h2_pal_ipv6_local.h"
#include "h2_desktop_platform.h"
#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#elif defined(__linux__)
#include "h2_linux_platform.h"
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the actual result callbacks and ESP console writer, without an SDK
 * or a network run. These controlled records never establish qualification. */
#include "../src/h2_pal_ipv6_e2e.c"
#define report_log ipv6_console_log
#include "../../../../targets/h2loader_tar_zlib/pal-ipv6/devkit/main/main.c"
#undef report_log

static const char session[] = "0123456789abcdef0123456789abcdef";
typedef struct fixture {
  const h2_pal_task_api_t *real_task;
  const h2_pal_sync_api_t *sync;
  h2_pal_semaphore_t *entered, *release;
  unsigned allocations, frees, joins, task_starts;
  unsigned listener_closes, client_closes;
  int fail_alloc, listen_error, start_error, http_error;
  unsigned join_failures;
  int served, listener_live, worker_completed;
  char reply[256];
  size_t reply_size;
  int isolation, next_socket, alive[32], family[32];
  int listener4, listener6, negative_error, positive4_error;
  int negative_open_error, positive4_listen_error;
  int positive6_error, post6_error, wrong_peer_family_for;
  unsigned positive6_connects, negative_connects;
} fixture_t;

static void *allocate(void *user, size_t size) {
  fixture_t *f = user;
  if (f->fail_alloc)
    return NULL;
  void *value = malloc(size);
  assert(value);
  ++f->allocations;
  return value;
}
static void release_memory(void *user, void *value) {
  fixture_t *f = user;
  assert(value && f->allocations);
  --f->allocations;
  ++f->frees;
  free(value);
}
static int listen_socket(void *user, h2_pal_net_family_t family, uint16_t port,
                         const h2_pal_net_bind_t *bind, int *out,
                         h2_pal_net_addr_t *address) {
  fixture_t *f = user;
  (void)bind;
  if (f->listen_error)
    return f->listen_error;
  if (f->isolation && family == H2_PAL_NET_FAMILY_IPV4 && f->positive4_listen_error)
    return f->positive4_listen_error;
  memset(address, 0, sizeof(*address));
  address->family = family;
  address->port = port ? port : 32100u;
  if (!f->isolation) {
    assert(family == H2_PAL_NET_FAMILY_IPV4);
    *out = 100;
    f->listener_live = 1;
  } else {
    *out = ++f->next_socket;
    assert(*out < 32);
    f->alive[*out] = 1;
    f->family[*out] = family;
    if (family == H2_PAL_NET_FAMILY_IPV4)
      f->listener4 = *out;
    else
      f->listener6 = *out;
  }
  return H2_PAL_OK;
}
static int open_socket(void *user, h2_pal_net_family_t family,
                       const h2_pal_net_bind_t *bind, int *out) {
  fixture_t *f = user;
  (void)bind;
  assert(f->isolation);
  if (family == H2_PAL_NET_FAMILY_IPV4 && f->listener4 < 0 &&
      f->negative_open_error)
    return f->negative_open_error;
  *out = ++f->next_socket;
  assert(*out < 32);
  f->alive[*out] = 1;
  f->family[*out] = family;
  return H2_PAL_OK;
}
static int open_default_socket(void *user, h2_pal_net_family_t family, int *out) {
  return open_socket(user, family, NULL, out);
}
static int connect_socket(void *user, int socket,
                          const h2_pal_net_addr_t *address, uint32_t timeout) {
  fixture_t *f = user;
  (void)timeout;
  assert(f->isolation && f->alive[socket]);
  assert(f->family[socket] == (int)address->family);
  if (address->family == H2_PAL_NET_FAMILY_IPV4) {
    if (f->listener4 < 0) {
      assert(f->listener6 >= 0 && f->alive[f->listener6]);
      ++f->negative_connects;
      return f->negative_error;
    }
    return f->positive4_error;
  }
  if (++f->positive6_connects > 1u) {
    assert(f->listener4 < 0 && f->listener6 >= 0 && f->negative_connects == 1u);
    return f->post6_error;
  }
  return f->positive6_error;
}
static int accept_socket(void *user, int listener, int *out,
                         h2_pal_net_addr_t *address, uint32_t timeout) {
  fixture_t *f = user;
  (void)timeout;
  memset(address, 0, sizeof(*address));
  if (f->isolation) {
    assert(f->alive[listener]);
    *out = ++f->next_socket;
    f->alive[*out] = 1;
    f->family[*out] = f->family[listener];
    address->family = (h2_pal_net_family_t)f->family[listener];
    if (f->wrong_peer_family_for == (int)address->family)
      address->family = address->family == H2_PAL_NET_FAMILY_IPV4
                            ? H2_PAL_NET_FAMILY_IPV6
                            : H2_PAL_NET_FAMILY_IPV4;
  } else {
    assert(listener == 100 && f->listener_live);
    assert(h2_pal_semaphore_give(f->sync, f->entered) == H2_PAL_OK);
    assert(h2_pal_semaphore_take(f->sync, f->release, 5000u) == H2_PAL_OK);
    assert(f->listener_live);
    *out = 101;
    address->family = H2_PAL_NET_FAMILY_IPV4;
  }
  return H2_PAL_OK;
}
static int receive_bytes(void *user, int socket, uint8_t *bytes, size_t size,
                         uint32_t timeout) {
  fixture_t *f = user;
  (void)timeout;
  assert(socket == 101 && !f->served);
  char request[128];
  int length = snprintf(request, sizeof(request),
                        "GET /%s HTTP/1.1\r\nHost: localhost\r\n\r\n", session);
  assert(length > 0 && (size_t)length < size);
  memcpy(bytes, request, (size_t)length);
  f->served = 1;
  return length;
}
static int send_bytes(void *user, int socket, const uint8_t *bytes, size_t size,
                      uint32_t timeout) {
  fixture_t *f = user;
  (void)timeout;
  assert(socket == 101 && size < sizeof(f->reply));
  memcpy(f->reply, bytes, size);
  f->reply[size] = '\0';
  f->reply_size = size;
  return (int)size;
}
static void close_socket(void *user, int socket) {
  fixture_t *f = user;
  if (f->isolation) {
    assert(socket >= 0 && socket < 32 && f->alive[socket]);
    f->alive[socket] = 0;
    if (socket == f->listener4)
      f->listener4 = -1;
    if (socket == f->listener6)
      f->listener6 = -1;
  } else if (socket == 100) {
    assert(f->listener_live);
    assert(!f->task_starts || f->worker_completed);
    f->listener_live = 0;
    ++f->listener_closes;
  } else {
    assert(socket == 101);
    ++f->client_closes;
  }
}
static int start_task(void *user, const h2_pal_task_options_t *options,
                      h2_pal_task_entry_t entry, void *context,
                      h2_pal_task_t **out) {
  fixture_t *f = user;
  if (f->start_error)
    return f->start_error;
  int rc = h2_pal_task_start(f->real_task, options, entry, context, out);
  if (rc == H2_PAL_OK) {
    ++f->task_starts;
    assert(h2_pal_semaphore_take(f->sync, f->entered, 5000u) == H2_PAL_OK);
  }
  return rc;
}
static int join_task(void *user, h2_pal_task_t *task) {
  fixture_t *f = user;
  ++f->joins;
  if (f->join_failures) {
    --f->join_failures;
    return H2_PAL_ERR_IO;
  }
  int rc = h2_pal_task_join(f->real_task, task);
  if (rc == H2_PAL_OK)
    f->worker_completed = 1;
  return rc;
}
static int http_call(void *user, const char *url) {
  fixture_t *f = user;
  assert(strstr(url, "http://localhost:32100/") == url);
  assert(strstr(url, session));
  return f->http_error;
}
static const h2_pal_mem_vtable_t memory = {
    .alloc = allocate, .free = release_memory};
static const h2_pal_task_vtable_t tasks = {.start = start_task, .join = join_task};
static const h2_pal_net_vtable_t network = {
    .tcp_listen = listen_socket, .tcp_open = open_default_socket,
    .tcp_open_bound = open_socket,
    .tcp_connect = connect_socket, .tcp_accept = accept_socket,
    .tcp_recv = receive_bytes, .tcp_send_timeout = send_bytes,
    .close = close_socket};

static void setup(fixture_t *f, int blocked) {
  memset(f, 0, sizeof(*f));
  f->real_task = h2_desktop_platform_task_api();
  f->sync = h2_desktop_platform_sync_api();
  h2_pal_semaphore_config_t config = {.max_count = 1u};
  assert(h2_pal_semaphore_create(f->sync, &config, &f->entered) == H2_PAL_OK);
  config.initial_count = blocked ? 0u : 1u;
  assert(h2_pal_semaphore_create(f->sync, &config, &f->release) == H2_PAL_OK);
}
static void finish(fixture_t *f) {
  assert(f->allocations == 0u && !f->listener_live);
  assert(h2_pal_semaphore_destroy(f->sync, f->release) == H2_PAL_OK);
  assert(h2_pal_semaphore_destroy(f->sync, f->entered) == H2_PAL_OK);
}
static void test_http_lifecycle(unsigned first) {
  for (unsigned mode = first; mode < 6u; ++mode) {
    fixture_t f;
    setup(&f, mode >= 4u);
    h2_pal_mem_api_t mem = {&f, &memory};
    h2_pal_task_api_t task = {&f, &tasks};
    h2_pal_net_api_t net = {&f, &network};
    h2_runtime_t runtime = {.mem = &mem, .net = &net, .task = &task};
    char borrowed_session[sizeof(session)];
    memcpy(borrowed_session, session, sizeof(session));
    if (mode == 1u)
      f.fail_alloc = 1;
    if (mode == 2u)
      f.listen_error = H2_PAL_ERR_IO;
    if (mode == 3u)
      f.start_error = H2_PAL_ERR_TASK;
    if (mode >= 4u)
      f.join_failures = 2u;
    if (mode == 5u)
      f.http_error = H2_PAL_ERR_TLS_VERIFY;
    h2_pal_ipv6_cleanup_t *pending = NULL;
    int rc = h2_pal_ipv6_local_http(&runtime, borrowed_session, http_call, &f,
                                    &pending);
    if (mode == 0u) {
      assert(rc == H2_PAL_OK && !pending);
      assert(f.listener_closes == 1u && f.client_closes == 1u);
    } else if (mode < 4u) {
      assert(rc != H2_PAL_OK && !pending);
      assert(f.task_starts == 0u);
    } else {
      assert(rc == (mode == 5u ? H2_PAL_ERR_TLS_VERIFY : H2_PAL_ERR_IO));
      assert(pending && f.allocations == 1u && f.listener_closes == 0u);
      assert(h2_pal_ipv6_local_cleanup_error(pending) == H2_PAL_ERR_IO);
      assert(h2_pal_ipv6_local_cleanup(&pending) == H2_PAL_ERR_IO);
      assert(pending && f.allocations == 1u && f.listener_closes == 0u);
      assert(h2_pal_ipv6_local_http(&runtime, session, http_call, &f,
                                   &pending) == H2_PAL_ERR_INVALID_STATE);
      memset(borrowed_session, 'x', sizeof(borrowed_session));
      memset(&runtime, 0, sizeof(runtime));
      assert(h2_pal_semaphore_give(f.sync, f.release) == H2_PAL_OK);
      assert(h2_pal_ipv6_local_cleanup(&pending) == H2_PAL_OK);
      assert(!pending && f.listener_closes == 1u && f.client_closes == 1u);
      assert(f.reply_size > 32u && strstr(f.reply, session));
      unsigned joins = f.joins, frees = f.frees;
      assert(h2_pal_ipv6_local_cleanup(&pending) == H2_PAL_OK);
      assert(f.joins == joins && f.frees == frees);
    }
    finish(&f);
  }
}

static void test_socket_isolation(void) {
  const int connect_results[] = {H2_PAL_ERR_CLOSED, H2_PAL_ERR_IO,
      H2_PAL_ERR_UNSUPPORTED, H2_PAL_ERR_INVALID_ARG, H2_PAL_ERR_NO_MEMORY,
      H2_PAL_ERR_NO_SPACE, H2_PAL_ERR_TIMEOUT, H2_PAL_ERR_WOULD_BLOCK,
      H2_PAL_ERR_INVALID_STATE, H2_PAL_ERR_FORMAT, H2_PAL_OK};
  for (unsigned i = 0; i < sizeof(connect_results) / sizeof(connect_results[0]);
       ++i) {
    fixture_t f = {.isolation = 1, .listener4 = -1, .listener6 = -1,
                   .negative_error = connect_results[i]};
    h2_pal_net_api_t net = {&f, &network};
    int rc = h2_pal_ipv6_local_isolation(&net);
    int refused = f.negative_error == H2_PAL_ERR_IO ||
                  f.negative_error == H2_PAL_ERR_CLOSED;
    assert((rc == H2_PAL_OK) == refused);
    assert(f.negative_connects == 1u);
    assert(f.positive6_connects == (refused ? 2u : 1u));
    for (unsigned j = 0; j < 32u; ++j)
      assert(!f.alive[j]);
  }
  for (unsigned i = 0; i + 1u < sizeof(connect_results) / sizeof(connect_results[0]);
       ++i) {
    fixture_t f = {.isolation = 1, .listener4 = -1, .listener6 = -1,
                   .negative_error = H2_PAL_ERR_IO,
                   .negative_open_error = connect_results[i]};
    h2_pal_net_api_t net = {&f, &network};
    assert(h2_pal_ipv6_local_isolation(&net) == f.negative_open_error);
    assert(f.negative_connects == 0u && f.positive6_connects == 1u);
    for (unsigned j = 0; j < 32u; ++j)
      assert(!f.alive[j]);
  }
  for (unsigned i = 0; i < 7u; ++i) {
    fixture_t f = {.isolation = 1, .listener4 = -1, .listener6 = -1,
                   .negative_error = H2_PAL_ERR_CLOSED};
    if (i == 0u) f.listen_error = H2_PAL_ERR_IO;
    if (i == 1u) f.positive6_error = H2_PAL_ERR_IO;
    if (i == 2u) f.positive4_error = H2_PAL_ERR_IO;
    if (i == 3u) f.wrong_peer_family_for = H2_PAL_NET_FAMILY_IPV4;
    if (i == 4u) f.wrong_peer_family_for = H2_PAL_NET_FAMILY_IPV6;
    if (i == 5u) f.post6_error = H2_PAL_ERR_IO;
    if (i == 6u) f.positive4_listen_error = H2_PAL_ERR_IO;
    h2_pal_net_api_t net = {&f, &network};
    assert(h2_pal_ipv6_local_isolation(&net) != H2_PAL_OK);
    assert(f.negative_connects == (i == 5u ? 1u : 0u));
    if (i == 5u) assert(f.positive6_connects == 2u);
    for (unsigned j = 0; j < 32u; ++j)
      assert(!f.alive[j]);
  }
}

typedef struct output_fixture {
  char bytes[65536];
  size_t length;
  unsigned writes, flushes, confirms;
  int write_error, short_write, flush_error, concurrent;
} output_fixture_t;
static output_fixture_t output_fixture;
H2_ATOMIC_DEFINE_STATIC(int, output_active, 0);

static int output_write(void *user, const void *bytes, size_t length,
                          size_t *written, uint32_t timeout) {
  output_fixture_t *f = user;
  assert(timeout == 5000u && length >= 2u);
  ++f->writes;
  *written = 0u;
  if (f->write_error)
    return f->write_error;
  if (f->concurrent) {
    assert(h2_atomic_int_exchange(&output_active, 1, H2_ATOMIC_SEQ_CST) == 0);
    assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u) == H2_PAL_OK);
  }
  assert(f->length + length < sizeof(f->bytes));
  memcpy(f->bytes + f->length, bytes, length);
  f->length += length;
  f->bytes[f->length] = '\0';
  *written = length - (f->short_write ? 1u : 0u);
  return H2_PAL_OK;
}
static int output_flush(void *user) {
  output_fixture_t *f = user;
  ++f->flushes;
  if (f->concurrent)
    assert(h2_atomic_int_exchange(&output_active, 0, H2_ATOMIC_SEQ_CST) == 1);
  return f->flush_error;
}
static const h2_pal_uart_io_stream_vtable_t output_uart_vtable = {
    .write = output_write, .flush = output_flush};
static const h2_pal_usb_jtag_io_stream_vtable_t output_usb_vtable = {
    .write = output_write, .flush = output_flush};
static const h2_pal_uart_io_stream_api_t output_uart = {
    .user = &output_fixture, .vtable = &output_uart_vtable};
static const h2_pal_usb_jtag_io_stream_api_t output_usb = {
    .user = &output_fixture, .vtable = &output_usb_vtable};
static void reset_output(int usb) {
  memset(&output_fixture, 0, sizeof(output_fixture));
  h2_atomic_int_store(&report_error, H2_PAL_OK, H2_ATOMIC_SEQ_CST);
  console.uart = usb ? NULL : &output_uart;
  console.usb = usb ? &output_usb : NULL;
}
static int output_sleep(void *user, uint32_t ms) {
  (void)user;
  assert(ms == 40u);
  return H2_PAL_OK;
}
static int forbidden_log(void *user, h2_pal_log_level_t level,
                           const char *scope, const char *message) {
  (void)user; (void)level; (void)scope; (void)message;
  assert(!"The result sink must not replace the real Runtime logger");
  return H2_PAL_ERR_IO;
}
static int output_confirm(h2_runtime_t *runtime) {
  assert(runtime && output_fixture.writes >= 57u &&
         output_fixture.writes == output_fixture.flushes);
  assert(strstr(output_fixture.bytes, "H2_PAL_IPV6_SUMMARY ") != NULL);
  ++output_fixture.confirms;
  return H2_PAL_OK;
}
static int witness_request(void *user, const h2_pal_http_request_t *request,
                             h2_pal_http_response_t *response) {
  (void)user;
  assert(strstr(request->url.data, "/session/controlled-session/") != NULL);
  if (request->method == H2_PAL_HTTP_GET) {
    static const char pair[] = "{\"local_family\":6,\"remote_family\":6}";
    assert(request->response_buf_cap >= sizeof(pair));
    memcpy(request->response_buf, pair, sizeof(pair));
    response->status_code = 200;
  } else {
    assert(request->method == H2_PAL_HTTP_POST);
    response->status_code = 204;
  }
  return H2_PAL_OK;
}
static void concurrent_report(void *user) {
  const char *line = user;
  for (unsigned i = 0u; i < 20u; ++i)
    assert(h2_pal_log_write(&ipv6_console_log, H2_PAL_LOG_INFO,
                             "pal-ipv6", line) == H2_PAL_OK);
}
static void test_checked_output(void) {
  console.sync = h2_desktop_platform_sync_api();
  const h2_pal_mutex_config_t lock = {
      .allocator = h2_desktop_platform_default_allocator()};
  assert(h2_pal_mutex_create(console.sync, &lock, &console.mutex) == H2_PAL_OK);
  static const h2_pal_time_vtable_t clock = {.sleep_ms = output_sleep};
  static const h2_pal_log_vtable_t original_log = {.write = forbidden_log};
  const h2_pal_time_api_t time = {.vtable = &clock};
  const h2_pal_log_api_t log = {.vtable = &original_log};
  h2_runtime_t runtime = {.time = &time, .log = &log};
  h2_pal_ipv6_result_t result = {.passed = H2_PAL_IPV6_CASES};
  for (unsigned i = 0u; i < H2_PAL_IPV6_CASES; ++i)
    result.cases[i] = (h2_net_tls_case_result_t){.id = "controlled-output", .passed = 1};
  for (int usb = 0; usb <= 1; ++usb) {
    reset_output(usb);
    assert(confirm_after_report(&runtime, &result, H2_PAL_OK, output_confirm) == H2_PAL_OK);
    assert(output_fixture.writes == 57u && output_fixture.flushes == 57u);
    assert(output_fixture.confirms == 1u);
    assert(strstr(output_fixture.bytes, "H2_PAL_IPV6_SUMMARY ") != NULL);
    assert(runtime.log == &log);
    for (unsigned failure = 0u; failure < 3u; ++failure) {
      reset_output(usb);
      if (failure == 0u) output_fixture.write_error = H2_PAL_ERR_TIMEOUT;
      if (failure == 1u) output_fixture.short_write = 1;
      if (failure == 2u) output_fixture.flush_error = H2_PAL_ERR_IO;
      int expected = failure == 0u ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
      assert(confirm_after_report(&runtime, &result, H2_PAL_OK, output_confirm) == expected);
      assert(output_fixture.confirms == 0u);
      output_fixture.write_error = output_fixture.short_write = output_fixture.flush_error = 0;
      assert(emit_report("later READY", sizeof("later READY"), 11) == expected);
      assert(output_fixture.writes == 1u);
    }
  }
  reset_output(0);
  assert(confirm_after_report(&runtime, &result, H2_PAL_ERR_CLOSED, output_confirm) == H2_PAL_ERR_CLOSED);
  assert(output_fixture.confirms == 0u);
  reset_output(0);
  result.cleanup_error = H2_PAL_ERR_BUSY;
  assert(confirm_after_report(&runtime, &result, H2_PAL_OK, output_confirm) == H2_PAL_ERR_BUSY);
  assert(output_fixture.confirms == 0u);
  result.cleanup_error = 0;
  reset_output(0);
  result.retained_tasks = 1u;
  assert(confirm_after_report(&runtime, &result, H2_PAL_OK, output_confirm) == H2_PAL_ERR_INVALID_STATE);
  assert(output_fixture.confirms == 0u);
  result.retained_tasks = 0u;
  reset_output(0);
  runtime.log = &ipv6_console_log;
  h2_pal_ipv6_device_report(&runtime, &result);
  assert(output_fixture.writes == 57u);
  runtime.log = &log;
  const h2_pal_ipv6_config_t config = {.transport = {.runtime = &runtime}};
  assert(report_log(&config) == &log);
  reset_output(0);
  char too_long[600]; memset(too_long, 'x', sizeof(too_long)); too_long[599] = '\0';
  result.cases[0].id = too_long;
  assert(h2_pal_ipv6_device_report_with_log(&runtime, &ipv6_console_log, &result) == H2_PAL_ERR_TRUNCATED);
  assert(output_fixture.writes == 0u);
  webrtc_fixture_t witness = {.log = &ipv6_console_log};
  h2_pal_webrtc_e2e_case_result_t item = {.id = too_long};
  webrtc_report(&witness, &item);
  assert(witness.report_error == H2_PAL_ERR_TRUNCATED && output_fixture.writes == 0u);
  reset_output(0);
  static const h2_pal_http_vtable_t witness_http_vtable = {.request = witness_request};
  const h2_pal_http_api_t witness_http = {.vtable = &witness_http_vtable};
  witness = (webrtc_fixture_t){
      .client = {.http = &witness_http, .offer_url = "https://fixture/offer",
                 .session = "controlled-session"}, .log = &ipv6_console_log};
  assert(webrtc_close(&witness) == H2_PAL_OK);
  assert(witness.ipv6_pairs == 1u && witness.report_error == H2_PAL_OK);
  assert(strstr(output_fixture.bytes, "H2_PAL_IPV6_ICE_WITNESS status=200 rc=0 local6=1 remote6=1") != NULL);
  reset_output(0);
  witness.report_error = H2_PAL_OK;
  item.id = "controlled-nested";
  for (unsigned i = 0u; i < H2_PAL_WEBRTC_E2E_CASE_COUNT; ++i)
    webrtc_report(&witness, &item);
  assert(witness.report_error == H2_PAL_OK && output_fixture.writes == 43u);
  output_fixture.write_error = H2_PAL_ERR_CLOSED;
  webrtc_report(&witness, &item);
  output_fixture.write_error = H2_PAL_ERR_IO;
  webrtc_report(&witness, &item);
  assert(witness.report_error == H2_PAL_ERR_CLOSED);
  reset_output(0);
  result.cases[0].id = "controlled-output";
  static const char execution[] = "H2_PAL_IPV6_EXECUTION session=controlled boot_id=controlled";
  assert(emit_report(execution, sizeof(execution), sizeof(execution) - 1u) == H2_PAL_OK);
  assert(confirm_after_report(&runtime, &result, H2_PAL_OK, output_confirm) == H2_PAL_OK);
  unsigned before_ready = output_fixture.writes;
  output_fixture.flush_error = H2_PAL_ERR_TIMEOUT;
  static const char ready[] = "H2_PAL_IPV6_READY rc=0 confirm=0";
  assert(emit_report(ready, sizeof(ready), sizeof(ready) - 1u) == H2_PAL_ERR_TIMEOUT);
  assert(output_fixture.confirms == 1u && output_fixture.writes == before_ready + 1u);
  reset_output(0);
  assert(emit_report("partial", 4u, 7) == H2_PAL_ERR_TRUNCATED);
  assert(output_fixture.writes == 0u);
  reset_output(0);
  output_fixture.concurrent = 1;
  h2_pal_task_t *a = NULL, *b = NULL;
  const h2_pal_task_api_t *task = h2_desktop_platform_task_api();
  assert(h2_pal_task_start(task, NULL, concurrent_report, "record-a", &a) == H2_PAL_OK);
  assert(h2_pal_task_start(task, NULL, concurrent_report, "record-b", &b) == H2_PAL_OK);
  assert(h2_pal_task_join(task, a) == H2_PAL_OK);
  assert(h2_pal_task_join(task, b) == H2_PAL_OK);
  assert(output_fixture.writes == 40u && output_fixture.flushes == 40u);
  assert(h2_pal_mutex_destroy(console.sync, console.mutex) == H2_PAL_OK);
  console.mutex = NULL;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--retained-only") == 0) {
    test_http_lifecycle(4u);
    return 0;
  }
  test_http_lifecycle(0u);
  test_socket_isolation();
  test_checked_output();
#if defined(__APPLE__)
  assert(h2_pal_ipv6_local_isolation(h2_darwin_net_api()) == H2_PAL_OK);
#elif defined(__linux__)
  assert(h2_pal_ipv6_local_isolation(h2_linux_net_api()) == H2_PAL_OK);
#endif
  puts("IPv6 local peer: real late worker, failed/retried joins and exact family proof PASS");
  return 0;
}
