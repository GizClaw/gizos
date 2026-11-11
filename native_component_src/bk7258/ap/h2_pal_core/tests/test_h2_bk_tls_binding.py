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
static void bk_net_lifetime_drain_compat(h2_bk_tls_lifetime_t* m) { (void)m; }
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
  while (!atomic_load(&stop)) if (!h2_bk_tls_worker_step(&manager)) wait_ms(NULL, 1);
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
