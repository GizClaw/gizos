"""Execute the production ESP TLS lifecycle against gated SDK effects."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
OWNER = ROOT / "native_component_src/esp-idf6.x/h2_pal_core"


class ESPBoundedTLS(unittest.TestCase):
    def test_production_setup_and_socket_ownership(self):
        source = (OWNER / "src/h2_esp_platform_net.c").read_text()
        # Compile complete production TLS helpers and entry points. SDK stubs
        # gate real setup/handshake calls; assertions inspect runtime effects.
        helpers = source[source.index("#define H2_ESP_NET_TLS_SOCKET_MAX"):
                         source.index("static int family_to_lwip(")]
        address = source[source.index("static int family_to_lwip("):
                         source.index("static int net_has_reserved_suffix(")]
        sockets = source[source.index("static int esp_net_open_socket("):
                         source.index("typedef struct esp_net_ping_context {")]
        compiler = shutil.which("cc")
        self.assertTrue(compiler, "a native C compiler is required")
        text = ("#include \"net_tls_bounded_sdk.h\"\n#include \"h2_esp_platform_net_socket.c\"\n" + helpers + address + sockets +
                (OWNER / "tests/test_h2_esp_net_tls_bounded.c").read_text())
        with tempfile.TemporaryDirectory(prefix="h2-esp-tls-") as temporary:
            test = Path(temporary) / "tls.c"
            test.write_text(text)
            for version in (3, 4):
                with self.subTest(mbedtls_major=version):
                    binary = Path(temporary) / ("tls-" + str(version))
                    build = subprocess.run(
                        [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-D_DARWIN_C_SOURCE", "-D_DEFAULT_SOURCE", "-pthread",
                         "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                         "-DMBEDTLS_VERSION_MAJOR=" + str(version),
                         "-I" + str(ROOT / "pal/include"),
                         "-I" + str(OWNER / "src"), "-I" + str(OWNER / "tests"),
                         str(test), "-o", str(binary)], capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    for case in (
                        None,
                        "cancel-no-reaper",
                        "legacy-retry",
                        "legacy-sync-retry",
                        "plain-connect-retry",
                        "plain-accept-retry",
                        "plain-connect-no-tls-heap",
                        "plain-accept-no-tls-heap",
                        "native-init-budget",
                        "native-init-concurrent",
                        "ordinary-send-cancel",
                        "retry-next-tick",
                        "zero-connect-recovery",
                        "zero-accept-recovery",
                        "zero-ready-poll",
                        "concurrent-connect-recovery",
                        "concurrent-accept-recovery",
                        "concurrent-recovery-deadline",
                        "write-cancel-plain",
                        "write-cancel-tls",
                        "write-cancel-bounded",
                        "init-cancel-connect",
                        "init-cancel-accept",
                        "init-lease-deadline",
                        "poll-only-connect",
                        "poll-only-accept",
                        "poll-admission-retry",
                        "long-waiter-connect",
                        "long-waiter-accept",
                        "callback-waiter-connect",
                        "callback-waiter-accept",
                        "callback-after-all-close",
                        "waiter-before-callback",
                        "host-readiness-contract",
                    ):
                        command = [str(binary)] + ([] if case is None else [case])
                        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
                        self.assertEqual(result.returncode, 0, str(case) + "\n" + result.stdout + result.stderr)
                        for line in result.stdout.splitlines():
                            if line.startswith("ESP_BOOTSTRAP "):
                                print(line, flush=True)
                        print("mbedTLS " + str(version) + " process=" + (case or "default") + " PASS", flush=True)


if __name__ == "__main__":
    unittest.main()
