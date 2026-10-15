"""Compile the production socket opener with both BK IPv6 SDK profiles."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


PACKAGE = Path(__file__).resolve().parents[1]

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
