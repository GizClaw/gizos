"""Verify the pinned DHCP ACK splice and its failure-safe build-tree source."""

from pathlib import Path
import os
import shutil
from cmake_probe import run_command
import tempfile
import unittest


def cmake_tool():
    for path in (os.environ.get("CMAKE"), shutil.which("cmake"),
                 "/opt/homebrew/bin/cmake", "/usr/local/bin/cmake",
                 "C:/Program Files/CMake/bin/cmake.exe"):
        if path and Path(path).is_file():
            return path
    raise RuntimeError("CMake is needed for the BK ACK overlay regression")


SERVER = '''
static int send_response(int sock, struct sockaddr *addr, char *msg, int len)
{
    if (len < 0) return -1;
    dhcp_d("sent response, %d bytes", len);
\treturn 0;
}

#define ERROR_REFUSED 5
'''
TASK = '''
int dhcpd_thread;
int dhcp_start(void) {
    int ret;
    if (dhcpd_running || dhcp_server_init(intrfc_handle)) return -1;
    ret = rtos_create_psram_thread(&dhcpd_thread, 1);
    return ret;
}
'''
CMAKE = '''
cmake_minimum_required(VERSION 3.16)
project(BkDhcpAckOverlay C)
set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
set(H2_BK_CP_PSRAM_SERVICES ON)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sdk/dhcp-server.c" server_source)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sdk/dhcp-server-main.c" task_source)
string(REPLACE "\\r\\n" "\\n" server_source "${server_source}")
string(REPLACE "\\r\\n" "\\n" task_source "${task_source}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_cp_dhcp_buffer.c" "${server_source}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_cp_dhcp_task.c" "${task_source}")
add_library(lwip_intf_v2_1 STATIC
  "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_cp_dhcp_buffer.c"
  "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_cp_dhcp_task.c")
function(armino_component_get_property out component property)
  set(${out} ${component} PARENT_SCOPE)
endfunction()
include("${REPO_ROOT}/native_component_src/bk7258/shared/h2_bk_cp_lease.cmake")
'''


class BkDhcpAckOverlay(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        sdk = self.root / "sdk"
        sdk.mkdir()
        (sdk / "dhcp-server.c").write_bytes(SERVER.replace("\n", "\r\n").encode())
        (sdk / "dhcp-server-main.c").write_bytes(TASK.replace("\n", "\r\n").encode())
        (self.root / "CMakeLists.txt").write_text(CMAKE)

    def configure(self, name):
        build = self.root / name
        env = dict(os.environ, H2_GIZOS_ROOT=str(Path.cwd()))
        result = run_command([cmake_tool(), "-S", str(self.root), "-B", str(build)],
                                env=env)
        return build, result

    def test_ack_hook_is_on_success_only_and_sdk_is_pristine(self):
        originals = {p.name: p.read_bytes() for p in (self.root / "sdk").glob("*.c")}
        build, result = self.configure("patched")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        server = (build / "h2_bk_cp_dhcp_buffer.c").read_text()
        task = (build / "h2_bk_cp_dhcp_task.c").read_text()
        self.assertIn('h2_bk_wifi_lease_accept(ack->chaddr, lwip_ntohl(ack->yiaddr)', server)
        self.assertIn('sock == dhcps.sock', server)
        self.assertIn('option->length == 1u', server)
        self.assertIn('DHCP_MESSAGE_ACK', server)
        self.assertLess(server.index('if (len < 0) return -1;'),
                        server.index('h2_bk_wifi_lease_accept('))
        self.assertLess(server.index('h2_bk_wifi_lease_accept('), server.index('\treturn 0;'))
        self.assertIn('#include "h2_bk_wifi_lease.h"', server)
        self.assertLess(task.index('h2_bk_wifi_lease_reset();'),
                        task.index('ret = rtos_create_psram_thread('))
        self.assertEqual(originals, {p.name: p.read_bytes() for p in (self.root / "sdk").glob("*.c")})

    def test_missing_sdk_anchor_fails_closed(self):
        source = self.root / "sdk/dhcp-server.c"
        source.write_bytes(source.read_bytes().replace(b"static int send_response(",
                                                     b"static int renamed_response("))
        _, result = self.configure("drift")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("send_response changed", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
