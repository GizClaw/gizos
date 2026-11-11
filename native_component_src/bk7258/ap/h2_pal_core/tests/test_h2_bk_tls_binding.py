"""Exercise the production Net dispatch/close glue around retained TLS work."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[3]
PROBE = r'''
#define _POSIX_C_SOURCE 200809L
#include "h2_bk_tls_lifetime.h"
#include "os/h2_pal_mem.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static h2_bk_tls_lifetime_t manager;
static pthread_mutex_t metadata = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool released;
static atomic_bool entered, stop, worker_started;
static atomic_int closes, plaintext_calls, disposals, root_allocs, root_frees;
static size_t expected_ca_size = 3u;
static const uint8_t* original_ca;
static pthread_t caller_thread;
static void* psram_allocate(void* user, size_t bytes) {
  (void)user; assert(!pthread_equal(pthread_self(), caller_thread));
  atomic_fetch_add(&root_allocs, 1); return malloc(bytes);
}
static void psram_release(void* user, void* memory) {
  (void)user; assert(!pthread_equal(pthread_self(), caller_thread));
  assert(memory != NULL); atomic_fetch_add(&root_frees, 1); free(memory);
}
static const h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void) {
  static const h2_pal_mem_vtable_t vtable = {.alloc=psram_allocate, .free=psram_release};
  static const h2_pal_mem_api_t api = {.vtable=&vtable}; return &api;
}
static uintptr_t enter(void* u) { (void)u; assert(!pthread_mutex_lock(&metadata)); return 0; }
static void leave(void* u, uintptr_t g) { (void)u; (void)g; assert(!pthread_mutex_unlock(&metadata)); }
static uint64_t clock_ms(void* u) {
  (void)u; struct timespec now; assert(!clock_gettime(CLOCK_MONOTONIC, &now));
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}
static void wait_ms(void* u, uint32_t ms) {
  (void)u; struct timespec delay = {.tv_sec=ms/1000u, .tv_nsec=(long)(ms%1000u)*1000000L};
  (void)nanosleep(&delay, NULL);
}
static void wake(void* u) { (void)u; }
static h2_bk_tls_lifetime_t* bk_net_lifetime(void) { return &manager; }
static void dispose(void* u, int fd) { (void)u; assert(fd >= 0); atomic_fetch_add(&disposals, 1); }
static int closesocket(int fd) {
  assert(fd >= 0);
  if (fd == 17) assert(atomic_load(&entered) && released);
  atomic_fetch_add(&closes, 1); return 0;
}
static void close_fd(void* u, int fd) { (void)u; (void)closesocket(fd); }
static void bk_net_tls_dispose_context(void* u, int fd) { (void)u; (void)fd; assert(!"inline TLS destruction"); }
static int bk_net_track_tcp(int fd) { return h2_bk_tls_tcp_register(&manager, fd); }
static h2_pal_result_t bk_net_tls_wrap_impl(void* u, int fd,
    const h2_pal_net_tls_config_t* config, uint32_t timeout, int* out) {
  (void)u; assert(fd == 17 && timeout > 0);
  assert(!pthread_mutex_lock(&gate));
  atomic_store(&entered, true); assert(!pthread_cond_broadcast(&changed));
  while (!released) assert(!pthread_cond_wait(&changed, &gate));
  /* Caller has already freed these original inputs. Only the owned copy is safe. */
  assert(!strcmp(config->server_name, "owned.example"));
  assert(config->root_ca_pem_len == expected_ca_size);
  assert(config->root_ca_pem != original_ca);
  for (size_t i = 0; i < expected_ca_size; ++i) assert(config->root_ca_pem[i] == 0x42);
  assert(!pthread_mutex_unlock(&gate));
  *out = fd; return H2_PAL_OK;
}
static h2_pal_result_t wrap(void* u, int fd,
    const h2_pal_net_tls_config_t* config, uint64_t deadline) {
  (void)deadline; int out = -1;
  return bk_net_tls_wrap_impl(u, fd, config, 1, &out);
}
static h2_pal_result_t bk_net_tcp_connect_native(void* u, int fd,
    const h2_pal_net_addr_t* a, uint32_t t) {
  (void)u; (void)a; (void)t; assert(fd >= 0); return H2_PAL_OK;
}
static h2_pal_result_t bk_net_tcp_accept_native(void* u, int fd, int* out,
    h2_pal_net_addr_t* a, uint32_t t) {
  (void)u; (void)a; (void)t; assert(fd == 19); *out = 18; return H2_PAL_OK;
}
static int bk_net_tcp_send_native(void* u, int fd, const uint8_t* p, size_t n) {
  (void)u; (void)p; assert(fd >= 0); atomic_fetch_add(&plaintext_calls, 1); return (int)n;
}
static int bk_net_tcp_send_timeout_native(void* u, int fd, const uint8_t* p, size_t n, uint32_t t) {
  (void)t; return bk_net_tcp_send_native(u, fd, p, n);
}
static int bk_net_tcp_recv_native(void* u, int fd, uint8_t* p, size_t n, uint32_t t) {
  (void)u; (void)p; (void)n; (void)t; assert(fd >= 0); return H2_PAL_ERR_WOULD_BLOCK;
}
/* PRODUCTION_GLUE */
static const h2_pal_net_vtable_t net_vtable = {
  /* PRODUCTION_TLS_DISPATCH */
};
static const h2_pal_net_api_t net_api = {.vtable = &net_vtable};
static void* worker(void* u) {
  (void)u; atomic_store(&worker_started, true);
  while (!atomic_load(&stop))
    if (!h2_bk_tls_worker_step(&manager) && !h2_bk_tls_plain_worker_step(&manager)) wait_ms(NULL, 1);
  return NULL;
}
static void expect_closes(int count) {
  uint64_t deadline = clock_ms(NULL) + 2000;
  while (atomic_load(&closes) != count && clock_ms(NULL) < deadline) wait_ms(NULL, 1);
  assert(atomic_load(&closes) == count);
}
int main(void) {
  caller_thread = pthread_self();
  const h2_bk_tls_lifetime_ops_t ops = {
    .enter=enter, .leave=leave, .clock_ms=clock_ms, .wait_ms=wait_ms,
    .wake_worker=wake, .wrap=wrap, .dispose=dispose, .close_fd=close_fd,
    .alloc_root=bk_net_lifetime_alloc_root, .free_root=bk_net_lifetime_free_root};
  assert(h2_bk_tls_lifetime_init(&manager, &ops) == H2_PAL_OK);
  h2_bk_tls_lifetime_set_worker_ready(&manager, true);
  assert(bk_net_track_tcp(17) == H2_PAL_OK);
  pthread_t task; assert(!pthread_create(&task, NULL, worker, NULL));
  while (!atomic_load(&worker_started)) wait_ms(NULL, 1);
  char* host = malloc(32); uint8_t* ca = malloc(3);
  assert(host && ca); strcpy(host, "owned.example"); memset(ca, 0x42, 3);
  h2_pal_net_tls_config_t config = {.server_name=host, .root_ca_pem=ca,
    .root_ca_pem_len=3, .verify=H2_PAL_NET_TLS_VERIFY_REQUIRED};
  int out = 777; uint64_t before = clock_ms(NULL);
  assert(h2_pal_net_tls_wrap_bounded(&net_api, 17, &config, 100, &out) == H2_PAL_ERR_TIMEOUT);
  assert(clock_ms(NULL) - before < 2000 && atomic_load(&entered) && !released);
  assert(out == -1);
  free(host); free(ca); memset(&config, 0, sizeof(config));
  before = clock_ms(NULL);
  bk_net_close(NULL, 17);
  assert(clock_ms(NULL) - before < 100 && atomic_load(&closes) == 0);
  assert(bk_net_track_tcp(17) != H2_PAL_OK);
  const uint8_t bytes[] = {1,2,3};
  assert(bk_net_tcp_send_timeout(NULL, 17, bytes, sizeof(bytes), 1) < 0);
  assert(atomic_load(&plaintext_calls) == 0);
  assert(!pthread_mutex_lock(&gate)); released = true;
  assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&gate));
  expect_closes(1);
  assert(out == -1); /* worker must never publish into the returned stack output */
  assert(bk_net_track_tcp(17) == H2_PAL_OK);
  h2_pal_net_addr_t address = {0}; uint8_t scratch[1];
  assert(bk_net_tcp_connect(NULL, 17, &address, 1) == H2_PAL_OK);
  assert(bk_net_tcp_send(NULL, 17, bytes, sizeof(bytes)) == 3);
  assert(bk_net_tcp_send_timeout(NULL, 17, bytes, sizeof(bytes), 1) == 3);
  assert(bk_net_tcp_recv(NULL, 17, scratch, 1, 1) == H2_PAL_ERR_WOULD_BLOCK);
  /* Invalid compatibility setup does not poison a still-plain valid socket. */
  assert(h2_pal_net_tls_wrap(&net_api, 17, NULL, 1, &out) == H2_PAL_ERR_INVALID_ARG);
  assert(bk_net_tcp_send(NULL, 17, bytes, sizeof(bytes)) == 3);
  bk_net_close(NULL, 17); expect_closes(2);
  /* The real Net allocator hooks must run on the worker for a large root. */
  expected_ca_size = 96u * 1024u;
  host = malloc(32); ca = malloc(expected_ca_size);
  assert(host && ca); strcpy(host, "owned.example"); memset(ca, 0x42, expected_ca_size);
  original_ca = ca;
  config = (h2_pal_net_tls_config_t){.server_name=host, .root_ca_pem=ca,
    .root_ca_pem_len=expected_ca_size, .verify=H2_PAL_NET_TLS_VERIFY_REQUIRED};
  assert(bk_net_track_tcp(17) == H2_PAL_OK);
  assert(h2_pal_net_tls_wrap_bounded(&net_api, 17, &config, 1000, &out) == H2_PAL_OK && out == 17);
  free(host); free(ca); original_ca = NULL;
  assert(atomic_load(&root_allocs) == 1);
  bk_net_close(NULL, 17); expect_closes(3);
  assert(atomic_load(&root_frees) == 1);
  assert(bk_net_track_tcp(19) == H2_PAL_OK);
  assert(bk_net_tcp_accept(NULL, 19, &out, NULL, 1) == H2_PAL_OK && out == 18);
  bk_net_close(NULL, out); bk_net_close(NULL, 19); expect_closes(5);
  atomic_store(&stop, true); assert(!pthread_join(task, NULL));
  return 0;
}
'''


LIFECYCLE_PROBE = r'''
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
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>

static h2_bk_tls_lifetime_t manager;
static pthread_mutex_t metadata = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t events = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned entered;
static bool released, accept_entered, accept_released;
static atomic_bool stop;
static bool prefix_entered, prefix_released, close_entered, close_released;
static int blocked_close_fd = -1, retirement_probe_fd = -1, late_fd = -1;
static bool accept_fail, keep_blocker;
#define BEKEN_WAIT_FOREVER UINT32_MAX
static unsigned plain_tokens;
static int s_bk_plain_work;
static int rtos_set_semaphore(int* semaphore) {
  (void)semaphore; assert(!pthread_mutex_lock(&events));
  plain_tokens = 1u; assert(!pthread_cond_broadcast(&changed));
  assert(!pthread_mutex_unlock(&events)); return 0;
}
static int rtos_get_semaphore(int* semaphore, uint32_t ms) {
  (void)semaphore; assert(ms == BEKEN_WAIT_FOREVER);
  assert(!pthread_mutex_lock(&events));
  while (!plain_tokens && !atomic_load(&stop)) assert(!pthread_cond_wait(&changed, &events));
  if (atomic_load(&stop)) { assert(!pthread_mutex_unlock(&events)); pthread_exit(NULL); }
  plain_tokens = 0u; assert(!pthread_mutex_unlock(&events)); return 0;
}
static uintptr_t enter(void* u) { (void)u; assert(!pthread_mutex_lock(&metadata)); return 0; }
static void leave(void* u, uintptr_t g) { (void)u; (void)g; assert(!pthread_mutex_unlock(&metadata)); }
static uint64_t clock_ms(void* u) {
  (void)u; struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
  return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static void wait_ms(void* u, uint32_t ms) {
  (void)u; struct timespec t = {.tv_sec=ms/1000u, .tv_nsec=(long)(ms%1000u)*1000000L};
  (void)nanosleep(&t, NULL);
}
static void wake(void* u) { (void)u; (void)rtos_set_semaphore(&s_bk_plain_work); }
static h2_bk_tls_lifetime_t* bk_net_lifetime(void) { return &manager; }
static void bk_net_tls_dispose_context(void* u, int fd) { (void)u; (void)fd; }
static int closesocket(int fd) {
  if (fd == blocked_close_fd) {
    assert(!pthread_mutex_lock(&events)); close_entered=true;
    assert(!pthread_cond_broadcast(&changed));
    while (!close_released) assert(!pthread_cond_wait(&changed, &events));
    assert(!pthread_mutex_unlock(&events));
  }
  int rc = close(fd);
  if (fd == retirement_probe_fd) {
    /* The native number is reusable now, but the old generation must remain
     * excluded until the owner's metadata retirement completes. */
    assert(h2_bk_tls_tcp_register(&manager, fd) == H2_PAL_ERR_INVALID_STATE);
    h2_bk_tls_tcp_ref_t ref; bool tls;
    assert(h2_bk_tls_tcp_acquire(&manager, fd, &ref, &tls) == H2_PAL_ERR_CLOSED);
  }
  return rc;
}
static void close_fd(void* u, int fd) { (void)u; assert(!closesocket(fd)); }
static void dispose(void* u, int fd) { (void)u; (void)fd; }
static int bk_net_track_tcp(int fd) { return h2_bk_tls_tcp_register(&manager, fd); }
static h2_pal_result_t wrap(void* u, int fd, const h2_pal_net_tls_config_t* c, uint64_t d) {
  (void)u; (void)fd; (void)c; (void)d;
  assert(!pthread_mutex_lock(&events)); ++entered;
  assert(!pthread_cond_broadcast(&changed));
  while (!released) assert(!pthread_cond_wait(&changed, &events));
  assert(!pthread_mutex_unlock(&events)); return H2_PAL_OK;
}
static void* worker(void* u) {
  (void)u; while (!atomic_load(&stop))
    if (!h2_bk_tls_worker_step(&manager)) wait_ms(NULL, 1);
  return NULL;
}
static int wait_fd(int fd, int wr, uint32_t ms) {
  fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
  struct timeval t = {.tv_sec=ms/1000u, .tv_usec=(int)(ms%1000u)*1000};
  int rc = select(fd+1, wr ? NULL : &fds, wr ? &fds : NULL, NULL, &t);
  return rc > 0 ? H2_PAL_OK : rc == 0 ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
}
static int h2_bk_net_from_sockaddr(const struct sockaddr* a, h2_pal_net_addr_t* out) {
  (void)a; memset(out, 0, sizeof(*out)); return H2_PAL_OK;
}
static int gated_accept(int fd, struct sockaddr* a, socklen_t* n) {
  int got = accept_fail ? -1 : accept(fd, a, n);
  if (!accept_fail) assert(got >= 0);
  assert(!pthread_mutex_lock(&events));
  late_fd = got;
  if (!keep_blocker) blocked_close_fd = got;
  accept_entered = true;
  assert(!pthread_cond_broadcast(&changed));
  while (!accept_released) assert(!pthread_cond_wait(&changed, &events));
  assert(!pthread_mutex_unlock(&events));
  if (got < 0) errno = EINTR;
  return got;
}
static struct sockaddr_in connect_target;
static h2_pal_result_t bk_net_tcp_connect_native(void* u, int fd,
    const h2_pal_net_addr_t* address, uint32_t ms) {
  (void)u; (void)address; (void)ms;
  return connect(fd, (struct sockaddr*)&connect_target, sizeof(connect_target)) == 0
             ? H2_PAL_OK : H2_PAL_ERR_IO;
}
static int bk_net_tcp_send_timeout_native(void* u, int fd, const uint8_t* p,
                                           size_t n, uint32_t ms) {
  (void)u; (void)ms; assert(n >= 2u);
  int sent = (int)send(fd, p, 2u, 0); assert(sent == 2);
  assert(!pthread_mutex_lock(&events)); prefix_entered=true;
  assert(!pthread_cond_broadcast(&changed));
  while (!prefix_released) assert(!pthread_cond_wait(&changed, &events));
  assert(!pthread_mutex_unlock(&events)); return sent;
}
#define accept gated_accept
/* PRODUCTION_GLUE */
#undef accept
static void init(void) {
  const h2_bk_tls_lifetime_ops_t ops = {.enter=enter, .leave=leave,
    .clock_ms=clock_ms, .wait_ms=wait_ms, .wake_worker=wake,
    .wrap=wrap, .dispose=dispose, .close_fd=close_fd};
  assert(h2_bk_tls_lifetime_init(&manager, &ops) == H2_PAL_OK);
  h2_bk_tls_lifetime_set_worker_ready(&manager, true);
}
static int listener(struct sockaddr_in* address) {
  int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
  int reuse = 1; assert(!setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
  assert(!bind(fd, (struct sockaddr*)address, sizeof(*address)));
  socklen_t size = sizeof(*address); assert(!getsockname(fd, (struct sockaddr*)address, &size));
  assert(!listen(fd, 4)); assert(bk_net_track_idle_tcp(fd) == H2_PAL_OK); return fd;
}
static int connect_to(const struct sockaddr_in* address) {
  int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
  assert(!connect(fd, (const struct sockaddr*)address, sizeof(*address))); return fd;
}
static void* request(void* p) {
  int fd = *(int*)p, out = 123;
  h2_pal_net_tls_config_t c = {.server_name="example.test"};
  assert(h2_bk_tls_wrap_bounded(&manager, fd, &c, 100u, &out) == H2_PAL_ERR_TIMEOUT);
  assert(out == -1); return NULL;
}
static void* plain_worker(void* p) { bk_net_plain_worker(p); return NULL; }
static void* send_request(void* p) {
  assert(bk_net_tcp_send_timeout(NULL, *(int*)p, (const uint8_t*)"DATA", 4u, 100u) == 2);
  return NULL;
}
static int accepted_out, accepted_result;
static void* accept_request(void* p) {
  accepted_result = bk_net_tcp_accept(NULL, *(int*)p, &accepted_out, NULL, 1000u);
  return NULL;
}
int main(int argc, char** argv) {
  assert(argc == 2); init();
  pthread_t cleaner; assert(!pthread_create(&cleaner, NULL, plain_worker, &manager));
  struct sockaddr_in address = {.sin_family=AF_INET, .sin_addr.s_addr=htonl(0x7f000001u)};
  if (!strncmp(argv[1], "idle-", 5u)) {
    int blocker_listener = listener(&address), blocker_peer = connect_to(&address);
    int blocker = accept(blocker_listener, NULL, NULL); assert(blocker >= 0);
    assert(bk_net_track_tcp(blocker) == H2_PAL_OK); blocked_close_fd = blocker;
    bk_net_close(NULL, blocker);
    assert(!pthread_mutex_lock(&events));
    while (!close_entered) assert(!pthread_cond_wait(&changed, &events));
    assert(!pthread_mutex_unlock(&events));
    /* The only plain cleanup worker is now held inside another native close. */
    address.sin_port = 0;
    int listening = listener(&address), peer = -1;
    retirement_probe_fd = listening;
    h2_bk_tls_tcp_ref_t ref; bool tls;
    if (!strcmp(argv[1], "idle-fresh")) {
      bk_net_close(NULL, listening);
    } else if (!strcmp(argv[1], "idle-release")) {
      assert(h2_bk_tls_tcp_acquire(&manager, listening, &ref, &tls) == H2_PAL_OK);
      bk_net_close(NULL, listening);
      assert(h2_bk_tls_tcp_release(&manager, &ref) == H2_PAL_OK);
    } else {
      keep_blocker = true;
      accept_fail = !strcmp(argv[1], "idle-accept-error");
      peer = connect_to(&address);
      pthread_t caller; assert(!pthread_create(&caller, NULL, accept_request, &listening));
      assert(!pthread_mutex_lock(&events));
      while (!accept_entered) assert(!pthread_cond_wait(&changed, &events));
      assert(!pthread_mutex_unlock(&events));
      bk_net_close(NULL, listening);
      assert(!pthread_mutex_lock(&events)); accept_released = true;
      assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&events));
      assert(!pthread_join(caller, NULL));
      assert(accepted_result == H2_PAL_ERR_CLOSED && accepted_out == -1);
      if (late_fd >= 0) {
        assert(h2_bk_tls_tcp_acquire(&manager, late_fd, &ref, &tls) == H2_PAL_ERR_CLOSED);
        assert(bk_net_track_tcp(late_fd) == H2_PAL_ERR_INVALID_STATE);
      }
    }
    int rebound = socket(AF_INET, SOCK_STREAM, 0); assert(rebound >= 0);
    int reuse = 1; assert(!setsockopt(rebound, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    assert(bind(rebound, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(!close(rebound));
    assert(h2_bk_tls_tcp_acquire(&manager, listening, &ref, &tls) == H2_PAL_ERR_NOT_FOUND);
    /* Last-ref release must also retire a closed, never-connected socket. */
    int fresh = socket(AF_INET, SOCK_STREAM, 0); assert(fresh >= 0);
    assert(bk_net_track_idle_tcp(fresh) == H2_PAL_OK); retirement_probe_fd = fresh;
    assert(h2_bk_tls_tcp_acquire(&manager, fresh, &ref, &tls) == H2_PAL_OK);
    h2_bk_tls_tcp_ref_t stale = ref;
    bk_net_close(NULL, fresh);
    assert(h2_bk_tls_tcp_release(&manager, &ref) == H2_PAL_OK);
    assert(h2_bk_tls_tcp_acquire(&manager, fresh, &ref, &tls) == H2_PAL_ERR_NOT_FOUND);
    assert(fcntl(fresh, F_GETFD) == -1 && errno == EBADF);
    assert(h2_bk_tls_tcp_release(&manager, &stale) == H2_PAL_ERR_INVALID_STATE);
    retirement_probe_fd = -1;
    assert(!pthread_mutex_lock(&events)); assert(!close_released); close_released = true;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&events));
    assert(wait_fd(blocker_peer, 0, 100u) == H2_PAL_OK);
    char byte; assert(recv(blocker_peer, &byte, 1, 0) == 0); assert(!close(blocker_peer));
    bk_net_close(NULL, blocker_listener);
    if (peer >= 0) {
      if (late_fd >= 0) {
        assert(wait_fd(peer, 0, 100u) == H2_PAL_OK);
        assert(recv(peer, &byte, 1, 0) == 0);
      }
      assert(!close(peer));
    }
  } else if (!strcmp(argv[1], "plain")) {
    pthread_t workers[H2_BK_TLS_JOB_MAX], callers[H2_BK_TLS_JOB_MAX];
    int blocked[H2_BK_TLS_JOB_MAX];
    for (size_t i=0; i<H2_BK_TLS_JOB_MAX; ++i) {
      blocked[i] = socket(AF_INET, SOCK_STREAM, 0); assert(blocked[i] >= 0);
      assert(bk_net_track_tcp(blocked[i]) == H2_PAL_OK);
      assert(!pthread_create(&workers[i], NULL, worker, NULL));
      assert(!pthread_create(&callers[i], NULL, request, &blocked[i]));
    }
    assert(!pthread_mutex_lock(&events));
    while (entered != H2_BK_TLS_JOB_MAX) assert(!pthread_cond_wait(&changed, &events));
    assert(!pthread_mutex_unlock(&events));
    for (size_t i=0; i<H2_BK_TLS_JOB_MAX; ++i) assert(!pthread_join(callers[i], NULL));
    int listening = listener(&address);
    bk_net_close(NULL, listening);
    int rebound = socket(AF_INET, SOCK_STREAM, 0); assert(rebound >= 0);
    int reuse=1; assert(!setsockopt(rebound, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    assert(bind(rebound, (struct sockaddr*)&address, sizeof(address)) == 0);
    assert(!close(rebound));
    address.sin_port = 0; listening = listener(&address);
    int peer = connect_to(&address);
    int stream = accept(listening, NULL, NULL); assert(stream >= 0);
    assert(bk_net_track_tcp(stream) == H2_PAL_OK);
    assert(send(stream, "DATA", 4, 0) == 4); bk_net_close(NULL, stream);
    char bytes[4]; assert(recv(peer, bytes, sizeof(bytes), MSG_WAITALL) == 4);
    assert(!memcmp(bytes, "DATA", 4)); assert(wait_fd(peer, 0, 100u) == H2_PAL_OK);
    assert(recv(peer, bytes, sizeof(bytes), 0) == 0);
    assert(!close(peer)); bk_net_close(NULL, listening);
    /* A completed positive prefix survives concurrent close and last-ref
     * release; all eight TLS workers are still inside native wrap. */
    address.sin_port = 0; listening = listener(&address); peer = connect_to(&address);
    stream = accept(listening, NULL, NULL); assert(stream >= 0);
    assert(bk_net_track_tcp(stream) == H2_PAL_OK); blocked_close_fd = stream;
    pthread_t sender; assert(!pthread_create(&sender, NULL, send_request, &stream));
    assert(!pthread_mutex_lock(&events));
    while (!prefix_entered) assert(!pthread_cond_wait(&changed, &events));
    assert(!pthread_mutex_unlock(&events));
    uint64_t before = clock_ms(NULL); bk_net_close(NULL, stream);
    assert(clock_ms(NULL) - before < 100u);
    assert(!pthread_mutex_lock(&events)); prefix_released=true;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&events));
    before = clock_ms(NULL); assert(!pthread_join(sender, NULL));
    assert(clock_ms(NULL) - before < 100u);
    assert(!pthread_mutex_lock(&events));
    while (!close_entered) assert(!pthread_cond_wait(&changed, &events));
    assert(!pthread_mutex_unlock(&events));
    assert(bk_net_track_tcp(stream) == H2_PAL_ERR_INVALID_STATE);
    assert(!pthread_mutex_lock(&events)); close_released=true;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&events));
    assert(recv(peer, bytes, sizeof(bytes), MSG_WAITALL) == 2);
    assert(!memcmp(bytes, "DA", 2)); assert(recv(peer, bytes, sizeof(bytes), 0) == 0);
    assert(!close(peer)); bk_net_close(NULL, listening);
    address.sin_port = 0; listening = listener(&address); connect_target = address;
    stream = socket(AF_INET, SOCK_STREAM, 0); assert(stream >= 0);
    assert(bk_net_track_idle_tcp(stream) == H2_PAL_OK);
    h2_pal_net_addr_t destination = {0};
    assert(bk_net_tcp_connect(NULL, stream, &destination, 100u) == H2_PAL_OK);
    peer = accept(listening, NULL, NULL); assert(peer >= 0);
    assert(!pthread_mutex_lock(&events));
    close_entered=false; close_released=false; blocked_close_fd=stream;
    assert(!pthread_mutex_unlock(&events));
    before = clock_ms(NULL); bk_net_close(NULL, stream);
    assert(clock_ms(NULL) - before < 100u);
    assert(!pthread_mutex_lock(&events));
    while (!close_entered) assert(!pthread_cond_wait(&changed, &events));
    assert(!pthread_mutex_unlock(&events));
    assert(bk_net_track_tcp(stream) == H2_PAL_ERR_INVALID_STATE);
    /* Even a stuck native FIN close cannot strand an idle listener. */
    bk_net_close(NULL, listening);
    rebound = socket(AF_INET, SOCK_STREAM, 0); assert(rebound >= 0);
    assert(!setsockopt(rebound, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    assert(!bind(rebound, (struct sockaddr*)&address, sizeof(address))); assert(!close(rebound));
    assert(!pthread_mutex_lock(&events)); close_released=true;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&events));
    assert(wait_fd(peer, 0, 100u) == H2_PAL_OK);
    assert(recv(peer, bytes, sizeof(bytes), 0) == 0); assert(!close(peer));
    assert(!pthread_mutex_lock(&events)); released=true;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&events));
    for (size_t i=0; i<H2_BK_TLS_JOB_MAX; ++i) bk_net_close(NULL, blocked[i]);
    atomic_store(&stop, true);
    for (size_t i=0; i<H2_BK_TLS_JOB_MAX; ++i) assert(!pthread_join(workers[i], NULL));
    while (h2_bk_tls_worker_step(&manager)) {}
  } else {
    int listening = listener(&address), peer = connect_to(&address);
    pthread_t caller; assert(!pthread_create(&caller, NULL, accept_request, &listening));
    assert(!pthread_mutex_lock(&events));
    while (!accept_entered) assert(!pthread_cond_wait(&changed, &events));
    assert(!pthread_mutex_unlock(&events));
    bk_net_close(NULL, listening);
    assert(!pthread_mutex_lock(&events)); accept_released=true;
    assert(!pthread_cond_broadcast(&changed)); assert(!pthread_mutex_unlock(&events));
    assert(!pthread_join(caller, NULL));
    assert(accepted_result == H2_PAL_ERR_CLOSED && accepted_out == -1);
    /* Cancellation publication cannot wait for the late fd's graceful close. */
    assert(!pthread_mutex_lock(&events));
    while (!close_entered) assert(!pthread_cond_wait(&changed, &events));
    close_released=true; assert(!pthread_cond_broadcast(&changed));
    assert(!pthread_mutex_unlock(&events));
    while (h2_bk_tls_worker_step(&manager)) {}
    assert(wait_fd(peer, 0, 100u) == H2_PAL_OK);
    char byte; assert(recv(peer, &byte, 1, 0) == 0); assert(!close(peer));
  }
  atomic_store(&stop, true); wake(NULL); assert(!pthread_join(cleaner, NULL));
  puts("production plain close/late accept passed"); return 0;
}
'''


BOOTSTRAP_PROBE = r'''
#include "h2_bk_tls_lifetime.h"
#include "os/h2_pal_task.h"
#include "os/h2_pal_mem.h"
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define kNoErr 0
#define BEKEN_WAIT_FOREVER UINT32_MAX
#define H2_BK_NET_TLS_SOCKET_MAX H2_BK_TLS_JOB_MAX
typedef void* beken_semaphore_t;
typedef void* beken_mutex_t;
static beken_mutex_t s_bk_tls_mutex, s_bk_tls_io_mutexes[H2_BK_TLS_JOB_MAX];
static struct { int fd; } s_bk_tls_sockets[H2_BK_TLS_JOB_MAX];
static int s_bk_tls_mutex_ready;
static unsigned mutex_fail, allocation_fail, semaphore_fail, start_fail;
static unsigned allocations, task_starts, task_joins, native_closes, mutex_calls;
static unsigned semaphore_calls, semaphore_fail_at, mutex_fail_at;
static struct { unsigned maximum, tokens; } semaphores[2];
static unsigned semaphore_count, waits;
static jmp_buf worker_parked;
static h2_pal_task_entry_t plain_entry;
static void* plain_context;
static uint32_t rtos_enter_critical(void) { return 0; }
static void rtos_exit_critical(uint32_t g) { (void)g; }
static int rtos_init_mutex(beken_mutex_t* m) {
  ++mutex_calls;
  if (mutex_fail || mutex_calls == mutex_fail_at) {
    if (mutex_fail) --mutex_fail; *m=NULL; return -1;
  }
  *m=(void*)1; return 0;
}
static int rtos_deinit_mutex(beken_mutex_t* m) { *m=NULL; return 0; }
static int rtos_init_semaphore(beken_semaphore_t* s, int n) {
  assert(n == H2_BK_TLS_JOB_MAX || n == 1);
  ++semaphore_calls;
  if (semaphore_fail || semaphore_calls == semaphore_fail_at) {
    if (semaphore_fail) --semaphore_fail;
    *s=NULL; return -1;
  }
  assert(semaphore_count < 2u); semaphores[semaphore_count].maximum = (unsigned)n;
  *s = &semaphores[semaphore_count++]; return 0;
}
static int rtos_set_semaphore(beken_semaphore_t* s) {
  assert(*s);
  for (unsigned i=0; i<semaphore_count; ++i) if (*s == &semaphores[i]) {
    if (semaphores[i].tokens < semaphores[i].maximum) ++semaphores[i].tokens;
    return 0;
  }
  assert(!"unknown semaphore"); return -1;
}
static int rtos_get_semaphore(beken_semaphore_t* s, uint32_t t) {
  assert(*s && t == BEKEN_WAIT_FOREVER); ++waits;
  for (unsigned i=0; i<semaphore_count; ++i) if (*s == &semaphores[i]) {
    if (semaphores[i].tokens) { --semaphores[i].tokens; return 0; }
    longjmp(worker_parked, 1);
  }
  assert(!"unknown semaphore"); return -1;
}
static void rtos_delay_milliseconds(uint32_t t) { (void)t; }
static uint64_t bk_net_now_ms(void) { return 0; }
static uint32_t bk_net_timeout_remaining_ms(uint64_t d) { return (uint32_t)d; }
static int closesocket(int fd) { assert(fd >= 0); ++native_closes; return 0; }
static void bk_net_tls_dispose_context(void* u, int fd) { (void)u; (void)fd; }
static h2_pal_result_t bk_net_tls_wrap_impl(void* u, int fd,
    const h2_pal_net_tls_config_t* c, uint32_t t, int* out) {
  (void)u; (void)c; (void)t; *out=fd; return H2_PAL_OK;
}
static void* allocate(void* u, size_t bytes) {
  (void)u; ++allocations;
  if (allocation_fail) { --allocation_fail; return NULL; } return malloc(bytes);
}
static void release(void* u, void* p) { (void)u; free(p); }
static const h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void) {
  static const h2_pal_mem_vtable_t v = {.alloc=allocate, .free=release};
  static const h2_pal_mem_api_t a = {.vtable=&v}; return &a;
}
static bool h2_bk_task_policy_is_configured(void) { return true; }
static const h2_pal_task_api_t* h2_bk_platform_task_api(void);
static int task_start(void* u, const h2_pal_task_options_t* o,
    h2_pal_task_entry_t f, void* c, h2_pal_task_t** out) {
  (void)u;
  assert((!strcmp(o->name, "$bk/net/tls") && o->min_stack_size == 16384u) ||
         (!strcmp(o->name, "$bk/net/plain-close") && o->min_stack_size == 4096u));
  assert(f && c); ++task_starts;
  if (start_fail && task_starts == start_fail) { *out=NULL; return H2_PAL_ERR_NO_MEMORY; }
  if (!strcmp(o->name, "$bk/net/plain-close")) { plain_entry=f; plain_context=c; }
  *out=(h2_pal_task_t*)(uintptr_t)(task_starts+1u); return H2_PAL_OK;
}
static int task_join(void* u, h2_pal_task_t* t) {
  (void)u; assert(t); ++task_joins; return H2_PAL_ERR_TIMEOUT;
}
static const h2_pal_task_api_t* h2_bk_platform_task_api(void) {
  static const h2_pal_task_vtable_t v = {.start=task_start, .join=task_join};
  static const h2_pal_task_api_t a = {.vtable=&v}; return &a;
}
/* PRODUCTION_INIT */
/* PRODUCTION_BOOTSTRAP */
int main(int argc, char** argv) {
  assert(argc == 2);
  if (!strcmp(argv[1], "mutex")) mutex_fail=1;
  if (!strcmp(argv[1], "io-mutex")) mutex_fail_at=3;
  if (!strcmp(argv[1], "allocation")) allocation_fail=1;
  if (!strcmp(argv[1], "semaphore")) semaphore_fail=1;
  if (!strcmp(argv[1], "tls-semaphore")) semaphore_fail_at=2;
  if (!strcmp(argv[1], "start")) start_fail=4;
  if (!strcmp(argv[1], "cleanup-start")) start_fail=1;
  bk_net_lifetime_prepare();
  h2_bk_tls_lifetime_t* retained = s_bk_tls_lifetime;
  h2_pal_task_t* retained_workers[H2_BK_TLS_JOB_MAX];
  memcpy(retained_workers, s_bk_tls_workers, sizeof(retained_workers));
  if (retained) {
    assert(h2_bk_tls_tcp_register(retained, 33) == H2_PAL_OK);
    assert(h2_bk_tls_tcp_register(retained, 34) == H2_PAL_OK);
    assert(h2_bk_tls_tcp_close(retained, 34) == H2_PAL_OK);
  }
  bk_net_lifetime_prepare();
  for (size_t i=0; i<H2_BK_TLS_JOB_MAX; ++i)
    if (retained_workers[i]) assert(s_bk_tls_workers[i] == retained_workers[i]);
  assert(s_bk_tls_lifetime != NULL && s_bk_tls_lifetime->worker_ready);
  if (retained) {
    assert(s_bk_tls_lifetime == retained);
    h2_bk_tls_tcp_ref_t ref; bool tls;
    assert(h2_bk_tls_tcp_acquire(retained, 33, &ref, &tls) == H2_PAL_OK);
    bk_net_lifetime_release(retained, &ref);
  }
  assert(task_joins == 0u);
  assert(task_starts == H2_BK_TLS_JOB_MAX + 1u + (start_fail ? 1u : 0u));
  /* Exercise functions normally reached through the production callback table. */
  assert(bk_net_lifetime() == s_bk_tls_lifetime);
  if (!retained) {
    assert(h2_bk_tls_tcp_register(s_bk_tls_lifetime, 34) == H2_PAL_OK);
    assert(h2_bk_tls_tcp_close(s_bk_tls_lifetime, 34) == H2_PAL_OK);
  }
  /* Start the actual production cleanup entry only after bootstrap completes.
   * It must drain old backlog, consume its retained wake token and then park. */
  assert(native_closes == 0u && plain_entry && plain_context == s_bk_tls_lifetime);
  if (!setjmp(worker_parked)) plain_entry(plain_context);
  assert(native_closes == 1u && waits >= 2u);
  assert(bk_net_track_tcp(35) == H2_PAL_OK);
  free(s_bk_tls_lifetime);
  puts("production bootstrap recovery passed"); return 0;
}
'''


def function(source, name):
    match = re.search(r"static (?:void\*|void|int|h2_pal_result_t)\s+" + name + r"\([^;{]*\)\s*\{", source)
    if match is None:
        raise AssertionError("Missing production function " + name)
    opening = source.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class BoundedBinding(unittest.TestCase):
    def compile_and_run(self, unit, arguments):
        compiler = shutil.which("cc")
        self.assertTrue(compiler)
        with tempfile.TemporaryDirectory(prefix="h2-bk-net-lifecycle-") as directory:
            probe = Path(directory) / "probe.c"
            binary = Path(directory) / "probe"
            probe.write_text(unit)
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-pthread", "-fsanitize=address,undefined",
                                    "-I", str(PACKAGE / "src"), "-I", str(REPO / "pal/include"),
                                    str(probe), str(PACKAGE / "src/h2_bk_tls_lifetime.c"),
                                    "-o", str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for argument in arguments:
                with self.subTest(scenario=argument):
                    result = subprocess.run([str(binary), argument], capture_output=True,
                                            text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_plain_close_and_accept_publication(self):
        source = (PACKAGE / "src/h2_bk_platform_net.c").read_text()
        names = ["bk_net_track_idle_tcp", "bk_net_plain_worker",
                 "bk_net_lifetime_release", "bk_net_tcp_connect", "bk_net_tcp_accept_native",
                 "bk_net_tcp_accept", "bk_net_tcp_send_timeout", "bk_net_close"]
        unit = LIFECYCLE_PROBE.replace("/* PRODUCTION_GLUE */",
                                      "\n\n".join(function(source, name) for name in names))
        self.compile_and_run(unit, ["plain", "accept", "idle-release", "idle-fresh",
                                    "idle-accept-error", "idle-accept-late"])

    def test_bootstrap_recovers_without_replacing_live_state(self):
        source = (PACKAGE / "src/h2_bk_platform_net.c").read_text()
        start = source.index("static h2_bk_tls_lifetime_t* s_bk_tls_lifetime;")
        end = source.index("static int bk_net_track_tcp(int fd) {", start)
        unit = BOOTSTRAP_PROBE.replace("/* PRODUCTION_INIT */", function(source, "bk_net_tls_init"))
        unit = unit.replace("/* PRODUCTION_BOOTSTRAP */",
                            source[start:end] + function(source, "bk_net_track_tcp"))
        self.compile_and_run(unit, ["mutex", "io-mutex", "allocation", "semaphore",
                                    "tls-semaphore", "cleanup-start", "start"])

    def test_timeout_close_does_not_join_or_reuse_retained_fd(self):
        source = (PACKAGE / "src/h2_bk_platform_net.c").read_text()
        names = ["bk_net_lifetime_alloc_root", "bk_net_lifetime_free_root",
                 "bk_net_lifetime_release", "bk_net_tcp_connect", "bk_net_tcp_accept",
                 "bk_net_tcp_send", "bk_net_tcp_send_timeout", "bk_net_tcp_recv",
                 "bk_net_close", "bk_net_tls_wrap", "bk_net_tls_wrap_bounded"]
        unit = PROBE.replace("/* PRODUCTION_GLUE */", "\n\n".join(function(source, name) for name in names))
        dispatch = re.findall(r"^\s*\.(?:tls_wrap|tls_wrap_bounded)\s*=\s*bk_net_tls_wrap(?:_bounded)?,\s*$", source, re.M)
        self.assertEqual(len(dispatch), 2, "Both production TLS entries must be wired")
        unit = unit.replace("/* PRODUCTION_TLS_DISPATCH */", "\n".join(dispatch))
        compiler = shutil.which("cc")
        self.assertTrue(compiler)
        with tempfile.TemporaryDirectory(prefix="h2-bk-tls-binding-") as directory:
            probe = Path(directory) / "binding.c"
            binary = Path(directory) / "binding"
            probe.write_text(unit)
            build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-pthread", "-fsanitize=address,undefined",
                                    "-I", str(PACKAGE / "src"), "-I", str(REPO / "pal/include"),
                                    str(probe), str(PACKAGE / "src/h2_bk_tls_lifetime.c"),
                                    "-o", str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
