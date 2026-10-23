"""Compile the production socket opener with both BK IPv6 SDK profiles."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[3]

FAMILY_PROBE = r"""
#include "h2_bk_net_addr.h"
#include "h2/pal/net/h2_pal_netif.h"
#include <assert.h>
#include <string.h>
#include <unistd.h>
#define closesocket close
static void set_recv_timeout(int fd, uint32_t timeout) {
  (void)fd;
  (void)timeout;
}
static int bk_net_bind_interface(int fd, const h2_pal_netif_ref_t *ref) {
  assert(fd >= 0 && ref != NULL);
  return H2_PAL_OK;
}

/* PRODUCTION_ADDRESS_CONVERSION */
/* PRODUCTION_SOCKET_OPENERS */

int main(void) {
  const h2_pal_net_family_t invalid[] = {
      H2_PAL_NET_FAMILY_ANY, (h2_pal_net_family_t)99,
      (h2_pal_net_family_t)-1};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    int fd = 123;
    h2_pal_net_addr_t address = {0};
    assert(bk_net_tcp_open(NULL, invalid[i], &fd) == H2_PAL_ERR_INVALID_ARG);
    assert(fd == -1);
    fd = 123;
    assert(bk_net_udp_open(NULL, invalid[i], 0, &fd, &address) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(fd == -1);
    const h2_pal_netif_ref_t netif = {0};
    h2_pal_net_bind_t bindings[] = {
        {.type = H2_PAL_NET_BIND_DEFAULT},
        {.type = H2_PAL_NET_BIND_SOURCE_ADDR, .source_addr.family = invalid[i]},
        {.type = H2_PAL_NET_BIND_NETIF, .netif = &netif}};
    for (size_t j = 0; j < sizeof(bindings) / sizeof(bindings[0]); ++j) {
      fd = 123;
      assert(bk_net_tcp_open_bound(NULL, invalid[i], &bindings[j], &fd) ==
             H2_PAL_ERR_INVALID_ARG);
      assert(fd == -1);
      fd = 123;
      assert(bk_net_udp_open_bound(NULL, invalid[i], 0, &bindings[j], &fd,
                                   &address) == H2_PAL_ERR_INVALID_ARG);
      assert(fd == -1 && address.family == H2_PAL_NET_FAMILY_ANY);
    }
  }
  const h2_pal_net_family_t families[] = {
      H2_PAL_NET_FAMILY_IPV4, H2_PAL_NET_FAMILY_IPV6};
  for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
    int expected = !LWIP_IPV6 && families[i] == H2_PAL_NET_FAMILY_IPV6
                       ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_OK;
    int fd = 123;
    assert(bk_net_tcp_open(NULL, families[i], &fd) == expected);
    if (expected == H2_PAL_OK) { assert(fd >= 0); close(fd); }
    else assert(fd == -1);
    fd = 123;
    h2_pal_net_addr_t address = {0};
    assert(bk_net_udp_open(NULL, families[i], 0, &fd, &address) == expected);
    if (expected == H2_PAL_OK) {
      assert(fd >= 0 && address.family == families[i] && address.port != 0);
      close(fd);
    } else assert(fd == -1);
  }
  return 0;
}
"""

PROBE = r"""
#include <assert.h>
#include <stddef.h>
#define AF_INET 2
#define AF_INET6 10
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
/* lwIP omits these constants entirely in an IPv4-only SDK build. */
#if LWIP_IPV6
#define IPPROTO_IPV6 41
#define IPV6_V6ONLY 26
#endif
static int socket_result = 17;
static int socket_calls, option_calls, close_calls;
static int socket(int family, int type, int protocol) {
  assert(family == AF_INET || family == AF_INET6);
  assert(type == SOCK_STREAM || type == SOCK_DGRAM);
  assert(protocol == 0);
  ++socket_calls;
  return socket_result;
}
#if LWIP_IPV6
static int option_result;
static int setsockopt(int fd, int level, int option, const void *value,
                     size_t size) {
  assert(fd == 17 && level == IPPROTO_IPV6 && option == IPV6_V6ONLY);
  assert(size == sizeof(int) && *(const int *)value == 1);
  ++option_calls;
  return option_result;
}
static int closesocket(int fd) {
  assert(fd == 17);
  ++close_calls;
  return 0;
}
#endif

/* PRODUCTION_SOCKET_OPENER */

int main(void) {
  assert(bk_net_open_socket(AF_INET, SOCK_DGRAM) == 17);
  assert(socket_calls == 1 && option_calls == 0 && close_calls == 0);
  socket_result = -1;
  assert(bk_net_open_socket(AF_INET, SOCK_STREAM) == -1);
  assert(socket_calls == 2 && option_calls == 0 && close_calls == 0);
#if LWIP_IPV6
  socket_result = 17;
  assert(bk_net_open_socket(AF_INET6, SOCK_DGRAM) == 17);
  assert(option_calls == 1 && close_calls == 0);
  option_result = -1;
  assert(bk_net_open_socket(AF_INET6, SOCK_STREAM) == -1);
  assert(option_calls == 2 && close_calls == 1);
  socket_result = -1;
  assert(bk_net_open_socket(AF_INET6, SOCK_DGRAM) == -1);
  assert(option_calls == 2 && close_calls == 1);
#endif
  return 0;
}
"""


class SocketProfiles(unittest.TestCase):
    def test_family_validation_uses_real_socket_openers(self):
        source = (PACKAGE / "src/h2_bk_platform_net.c").read_text()
        address = (PACKAGE / "src/h2_bk_net_addr.c").read_text()
        conversions = address[address.index("int h2_bk_net_family("):
                              address.index("static int net_has_reserved_suffix(")]
        pieces = []
        for start, end in [
            ("static int bk_net_open_socket(", "static int bk_net_bind_interface("),
            ("static int bk_net_udp_open(", "static int bk_net_udp_sendto("),
            ("static int bk_net_tcp_open(", "static h2_pal_result_t bk_net_tcp_connect(")]:
            pieces.append(source[source.index(start):source.index(end)])
        probe = FAMILY_PROBE.replace("/* PRODUCTION_ADDRESS_CONVERSION */", conversions)
        probe = probe.replace("/* PRODUCTION_SOCKET_OPENERS */", "\n".join(pieces))
        compiler = shutil.which("cc")
        self.assertTrue(compiler, "a native C compiler is required")
        with tempfile.TemporaryDirectory(prefix="h2-bk-net-family-") as directory:
            root = Path(directory)
            unit = root / "probe.c"
            unit.write_text(probe)
            for enabled in (0, 1):
                with self.subTest(ipv6=enabled):
                    binary = root / f"family-{enabled}"
                    build = subprocess.run(
                        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                         "-fsanitize=address,undefined", f"-DLWIP_IPV6={enabled}",
                         "-I", str(PACKAGE / "src"),
                         "-I", str(PACKAGE / "tests/ipv6_sdk"),
                         "-I", str(REPO / "libs/pal/include"),
                         str(unit), "-o", str(binary)], capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    run = subprocess.run([str(binary)], capture_output=True,
                                         text=True, timeout=5)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_ipv4_only_and_ipv6_enabled(self):
        source = (PACKAGE / "src/h2_bk_platform_net.c").read_text()
        start = source.index("static int bk_net_open_socket(")
        end = source.index("\nstatic int bk_net_bind_interface(", start)
        probe = PROBE.replace("/* PRODUCTION_SOCKET_OPENER */", source[start:end])
        compiler = shutil.which("cc")
        self.assertTrue(compiler, "a native C compiler is required")
        with tempfile.TemporaryDirectory(prefix="h2-bk-net-socket-") as directory:
            root = Path(directory)
            unit = root / "probe.c"
            unit.write_text(probe)
            for enabled in (0, 1):
                with self.subTest(ipv6=enabled):
                    binary = root / f"socket-{enabled}"
                    build = subprocess.run(
                        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                         f"-DLWIP_IPV6={enabled}", str(unit), "-o", str(binary)],
                        capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    run = subprocess.run([str(binary)], capture_output=True,
                                         text=True, timeout=5)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
