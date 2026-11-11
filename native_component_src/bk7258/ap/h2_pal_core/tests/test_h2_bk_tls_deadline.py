"""Compile the production BK clock and TLS loop with an unavailable libc clock."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[3]

PROBE = r"""
#include "net/h2_pal_net.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <sys/time.h>
#include "os/h2_pal_log.h"
#include "os/h2_pal_time.h"

#define MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS -0x7000
#define MBEDTLS_ERR_SSL_WANT_READ -0x6900
#define MBEDTLS_ERR_SSL_WANT_WRITE -0x6880
#define MBEDTLS_ERR_X509_CERT_VERIFY_FAILED -0x2700

#define MBEDTLS_PRIVATE(member) member
typedef struct { int state; } mbedtls_ssl_context;
typedef struct {
  mbedtls_ssl_context ssl;
  uint64_t random_ms, bio_ms;
} bk_net_tls_socket_t;
static uint64_t now_ms;
static int returns[8], completes[8], handshake_done;
static uint32_t costs[8];
static size_t calls, waits;
static uint32_t verify_flags, wait_cost, last_wait_budget;
static int wait_result;

static inline uint64_t bk_aon_rtc_get_us(void) { return now_ms * 1000u; }
static h2_pal_result_t fake_monotonic(void *user, uint64_t *out) {
  (void)user;
  *out = now_ms;
  return H2_PAL_OK;
}
static inline const h2_pal_time_api_t *h2_bk_platform_time_api(void) {
  static const h2_pal_time_vtable_t vtable = {.get_monotonic_ms = fake_monotonic};
  static const h2_pal_time_api_t api = {.vtable = &vtable};
  return &api;
}
/* Match the actual linked newlib/libnosys implementation, not a host clock. */
static inline int unavailable_gettimeofday(struct timeval *value, void *zone) {
  (void)value;
  (void)zone;
  errno = ENOSYS;
  return -1;
}
#define gettimeofday unavailable_gettimeofday

/* PRODUCTION_CLOCK */
static int mbedtls_ssl_handshake(mbedtls_ssl_context *context) {
  assert(context != NULL && calls < 8u);
  now_ms += costs[calls];
  int result = returns[calls];
  if (result == 0 && completes[calls]) handshake_done = 1;
  ++calls;
  return result;
}
#define mbedtls_ssl_handshake_step mbedtls_ssl_handshake
#define mbedtls_ssl_is_handshake_over(context) ((void)(context), handshake_done)
static inline void rtos_delay_milliseconds(uint32_t ms) { now_ms += ms; }
static uint32_t mbedtls_ssl_get_verify_result(mbedtls_ssl_context *context) {
  assert(context != NULL);
  return verify_flags;
}
static int bk_net_tls_wait(bk_net_tls_socket_t *socket, int result,
                           uint32_t timeout_ms) {
  assert(socket != NULL);
  assert(result == MBEDTLS_ERR_SSL_WANT_READ ||
         result == MBEDTLS_ERR_SSL_WANT_WRITE);
  ++waits;
  last_wait_budget = timeout_ms;
  now_ms += wait_cost;
  return wait_result;
}

/* PRODUCTION_HANDSHAKE */

static void reset(void) {
  now_ms = 0u;
  calls = waits = 0u;
  handshake_done = 0;
  verify_flags = wait_cost = last_wait_budget = 0u;
  wait_result = H2_PAL_OK;
  for (size_t i = 0u; i < 8u; ++i) {
    returns[i] = 0;
    completes[i] = 1;
    costs[i] = 0u;
  }
}

int main(void) {
  bk_net_tls_socket_t socket = {0};
  reset();
  struct timeval unavailable = {0};
  uint64_t monotonic = 123u;
  assert(gettimeofday(&unavailable, NULL) == -1 && errno == ENOSYS);
  assert(h2_pal_time_get_monotonic_ms(h2_bk_platform_time_api(), &monotonic) ==
         H2_PAL_OK);
  assert(monotonic == now_ms);

  /* A native crypto call can return success after the budget has elapsed. */
  reset();
  costs[0] = 16u;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) ==
         H2_PAL_ERR_TIMEOUT);
  assert(calls == 1u && waits == 0u);

  /* Equality is expired, not one more successful millisecond. */
  reset();
  costs[0] = 15u;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) ==
         H2_PAL_ERR_TIMEOUT);

  /* A ready descriptor does not extend a deadline spent waiting for it. */
  reset();
  returns[0] = MBEDTLS_ERR_SSL_WANT_READ;
  costs[0] = 2u;
  wait_cost = 13u;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) ==
         H2_PAL_ERR_TIMEOUT);
  assert(calls == 1u && waits == 1u && last_wait_budget == 13u);

  /* In-budget nonblocking progress still completes successfully. */
  reset();
  returns[0] = MBEDTLS_ERR_SSL_WANT_WRITE;
  costs[0] = 2u;
  costs[1] = 2u;
  wait_cost = 3u;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) == H2_PAL_OK);
  assert(calls == 2u && waits == 1u && last_wait_budget == 13u);

  /* A zero result can mean an intermediate handshake state, not completion. */
  reset();
  completes[0] = 0;
  costs[0] = costs[1] = 1u;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) == H2_PAL_OK);
  assert(calls == 2u && waits == 0u);

  reset();
  returns[0] = MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS;
  costs[0] = costs[1] = 1u;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) == H2_PAL_OK);
  assert(calls == 2u && waits == 0u);

  /* Preserve an absolute budget passed after configuration work. */
  reset();
  now_ms = 1000u;
  costs[0] = 16u;
  assert(bk_net_tls_handshake(&socket, 1015u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) ==
         H2_PAL_ERR_TIMEOUT);
  reset();
  now_ms = 15u;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) ==
         H2_PAL_ERR_TIMEOUT);
  assert(calls == 0u);

  reset();
  verify_flags = 1u;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) ==
         H2_PAL_ERR_TLS_VERIFY);

  reset();
  returns[0] = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) ==
         H2_PAL_ERR_TLS_VERIFY);

  reset();
  returns[0] = -123;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) == H2_PAL_ERR_IO);

  reset();
  returns[0] = MBEDTLS_ERR_SSL_WANT_READ;
  wait_result = H2_PAL_ERR_IO;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_REQUIRED) == H2_PAL_ERR_IO);

  /* Even the explicit test-only verify mode must honor its time budget. */
  reset();
  verify_flags = 1u;
  costs[0] = 16u;
  assert(bk_net_tls_handshake(&socket, 15u,
                             H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY) ==
         H2_PAL_ERR_TIMEOUT);
  return 0;
}
"""


class TLSDeadline(unittest.TestCase):
    def test_native_handshake_preserves_absolute_deadline(self):
        source = (PACKAGE / "src/h2_bk_platform_net.c").read_text()
        start = source.index("static h2_pal_result_t bk_net_tls_handshake(")
        end = source.index("\nstatic void set_recv_timeout(", start)
        clock_start = source.index("static uint64_t bk_net_now_ms(void) {")
        clock_end = source.index("\nstatic int bk_net_resolve_addr(", clock_start)
        probe = PROBE.replace("/* PRODUCTION_HANDSHAKE */", source[start:end])
        probe = probe.replace("/* PRODUCTION_CLOCK */", source[clock_start:clock_end])
        compiler = shutil.which("cc")
        self.assertTrue(compiler, "a native C compiler is required")
        with tempfile.TemporaryDirectory(prefix="h2-bk-tls-deadline-") as directory:
            root = Path(directory)
            unit, binary = root / "probe.c", root / "probe"
            unit.write_text(probe)
            build = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(REPO / "pal/include"),
                 str(unit), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True,
                                 text=True, timeout=5)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
