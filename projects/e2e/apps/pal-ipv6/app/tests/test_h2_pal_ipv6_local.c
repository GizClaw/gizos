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
    if (f->listener4 < 0)
      return f->negative_error;
    return f->positive4_error;
  }
  return H2_PAL_OK;
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
  const int failures[] = {H2_PAL_ERR_UNSUPPORTED, H2_PAL_ERR_INVALID_ARG,
      H2_PAL_ERR_NO_MEMORY, H2_PAL_ERR_TIMEOUT, H2_PAL_ERR_WOULD_BLOCK,
      H2_PAL_OK};
  for (unsigned i = 0; i <= sizeof(failures) / sizeof(failures[0]); ++i) {
    fixture_t f = {.isolation = 1, .listener4 = -1, .listener6 = -1,
                   .negative_error = H2_PAL_ERR_IO};
    if (i < sizeof(failures) / sizeof(failures[0]))
      f.negative_error = failures[i];
    h2_pal_net_api_t net = {&f, &network};
    int rc = h2_pal_ipv6_local_isolation(&net);
    assert((rc == H2_PAL_OK) == (f.negative_error == H2_PAL_ERR_IO));
    for (unsigned j = 0; j < 32u; ++j)
      assert(!f.alive[j]);
  }
  fixture_t f = {.isolation = 1, .listener4 = -1, .listener6 = -1,
                 .negative_error = H2_PAL_ERR_IO,
                 .positive4_error = H2_PAL_ERR_IO};
  h2_pal_net_api_t net = {&f, &network};
  assert(h2_pal_ipv6_local_isolation(&net) != H2_PAL_OK);
  for (unsigned j = 0; j < 32u; ++j)
    assert(!f.alive[j]);
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--retained-only") == 0) {
    test_http_lifecycle(4u);
    return 0;
  }
  test_http_lifecycle(0u);
  test_socket_isolation();
#if defined(__APPLE__)
  assert(h2_pal_ipv6_local_isolation(h2_darwin_net_api()) == H2_PAL_OK);
#elif defined(__linux__)
  assert(h2_pal_ipv6_local_isolation(h2_linux_net_api()) == H2_PAL_OK);
#endif
  puts("IPv6 local peer: real late worker, failed/retried joins and exact family proof PASS");
  return 0;
}
