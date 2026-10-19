"""Exercise the production BK CP PSRAM source correction against a pinned SDK shape."""

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
    raise RuntimeError("CMake is needed for the BK SDK overlay test")


SOURCES = {
    "lwip_intf_v2_1/dhcpd/dhcp-server.c": """
#include "dns.h"
#if !defined(BK_DHCP_PRIVATE_DNS)
#error The generated DHCP source included the public DNS header
#endif
#include <stddef.h>
extern void *os_malloc(size_t);
extern void *psram_malloc(size_t);
#define os_mem_alloc os_malloc
void *dhcp_buffer(void) { return os_mem_alloc(1024); }
""",
    "lwip_intf_v2_1/dhcpd/dhcp-server-main.c": """
extern int rtos_create_thread(void *, ...);
extern int rtos_create_psram_thread(void *, ...);
int dhcpd_thread;
int dhcp_start(void) { return rtos_create_thread(&dhcpd_thread, 1536); }
""",
    "bk_cli/cli_main.c": """
extern int rtos_create_thread(void *, ...);
extern int rtos_create_psram_thread(void *, ...);
int cli_thread_handle;
int cli_debug(void) { return rtos_create_thread(&cli_thread_handle, 4096); }
int cli_normal(void) { return rtos_create_thread(&cli_thread_handle, 3072); }
int cli_fallback(void) { return rtos_create_thread(&cli_thread_handle, 3072); }
""",
    "lwip_intf_v2_1/lwip-2.1.2/port/sys_arch.c": """
#include <stdint.h>
extern int rtos_create_sram_thread(void *, ...);
extern int rtos_create_psram_thread(void *, ...);
int sys_thread_test(const char *name) {
    void *CreatedTask = 0;
    int prio = 3, stacksize = 768;
    void *thread = 0, *arg = 0;
    int result;
    result = rtos_create_sram_thread(&CreatedTask, prio, name, thread, stacksize * sizeof(uint32_t), arg);
    return result;
}
""",
    "wpa_supplicant-2.10/src/common/wpa_psk_cache.c": """
extern int rtos_create_thread(void *, ...);
extern int rtos_create_psram_thread(void *, ...);
#define CONFIG_TASK_WPAS_PRIO 5
int wpa_pskcalc_thread_handle;
int start_psk_test(void) {
    return rtos_create_thread(&wpa_pskcalc_thread_handle, CONFIG_TASK_WPAS_PRIO,
                              "pskc", 0, 2048, 0);
}
""",
}

PROBE = """
#include <stddef.h>
#include <stdio.h>
static unsigned sram_buffers, psram_buffers, sram_threads, psram_threads;
static char sram_space[1024], psram_space[1024];
void *os_malloc(size_t size) { if (size != 1024) return NULL; ++sram_buffers; return sram_space; }
void *psram_malloc(size_t size) { if (size != 1024) return NULL; ++psram_buffers; return psram_space; }
int rtos_create_thread(void *handle, ...) { if (!handle) return -1; ++sram_threads; return 0; }
int rtos_create_sram_thread(void *handle, ...) { if (!handle) return -1; ++sram_threads; return 0; }
int rtos_create_psram_thread(void *handle, ...) { if (!handle) return -1; ++psram_threads; return 0; }
extern void *dhcp_buffer(void);
extern int dhcp_start(void), cli_debug(void), cli_normal(void), cli_fallback(void);
extern int sys_thread_test(const char *), start_psk_test(void);
int main(void) {
    if (!dhcp_buffer() || dhcp_start() || cli_debug() || cli_normal() || cli_fallback() ||
        sys_thread_test("tcp/ip") || sys_thread_test("other") || start_psk_test()) return 1;
#if EXPECT_PSRAM
    if (sram_buffers || sram_threads != 1 || psram_buffers != 1 || psram_threads != 6) return 2;
#else
    if (sram_buffers != 1 || sram_threads != 7 || psram_buffers || psram_threads) return 3;
#endif
    puts("BK_CP_PSRAM_SERVICES_PAIRED_AND_SDK_PRISTINE PASS");
    return 0;
}
"""

CMAKE = """
cmake_minimum_required(VERSION 3.16)
project(CpPsramServices C)
set(CMAKE_C_STANDARD 11)
if(NOT DEFINED CONFIG_PSRAM_AS_SYS_MEMORY)
  set(CONFIG_PSRAM_AS_SYS_MEMORY 1)
endif()
if(NOT DEFINED CONFIG_FREERTOS_SMP)
  set(CONFIG_FREERTOS_SMP 0)
endif()
add_library(lwip_intf_v2_1 STATIC
  sdk/components/lwip_intf_v2_1/dhcpd/dhcp-server.c
  sdk/components/lwip_intf_v2_1/dhcpd/dhcp-server-main.c
  sdk/components/lwip_intf_v2_1/lwip-2.1.2/port/sys_arch.c)
target_include_directories(lwip_intf_v2_1 BEFORE PRIVATE public)
add_library(bk_cli STATIC sdk/components/bk_cli/cli_main.c)
add_library(wpa_supplicant-2.10 STATIC sdk/components/wpa_supplicant-2.10/src/common/wpa_psk_cache.c)
function(armino_component_get_property out component property)
  set(${out} ${component} PARENT_SCOPE)
endfunction()
if(APPLY)
  include("$ENV{H2_GIZOS_ROOT}/native_component_src/bk7258/shared/h2_bk_cp_psram.cmake")
endif()
add_executable(probe probe.c)
target_compile_definitions(probe PRIVATE EXPECT_PSRAM=$<BOOL:${APPLY}>)
target_link_libraries(probe PRIVATE lwip_intf_v2_1 bk_cli wpa_supplicant-2.10)
"""


class CpPsramServices(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.repo = Path.cwd()
        self.sdk = self.root / "sdk/components"
        for relative, source in SOURCES.items():
            path = self.sdk / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(source.replace("\n", "\r\n").encode())
        private_dns = self.sdk / "lwip_intf_v2_1/dhcpd/dns.h"
        private_dns.write_text("#define BK_DHCP_PRIVATE_DNS 1\n")
        public_dns = self.root / "public/dns.h"
        public_dns.parent.mkdir(parents=True)
        public_dns.write_text("#define BK_PUBLIC_DNS 1\n")
        (self.root / "probe.c").write_text(PROBE)
        (self.root / "CMakeLists.txt").write_text(CMAKE)

    def configure(self, name, apply, extra=()):
        build = self.root / name
        env = dict(os.environ, H2_GIZOS_ROOT=str(self.repo), ARMINO_PATH=str(self.root / "sdk"))
        command = [cmake_tool(), "-S", str(self.root), "-B", str(build),
                   "-DAPPLY=" + ("ON" if apply else "OFF"), *extra]
        result = run_command(command, env=env)
        return build, result

    def check_probe(self, name, apply):
        originals = {key: (self.sdk / key).read_bytes() for key in SOURCES}
        build, result = self.configure(name, apply)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = run_command([cmake_tool(), "--build", str(build), "--config", "Debug"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        binary = next(path for path in (build / "probe", build / "probe.exe", build / "Debug/probe.exe")
                      if path.is_file())
        result = run_command([str(binary)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("BK_CP_PSRAM_SERVICES_PAIRED_AND_SDK_PRISTINE PASS", result.stdout)
        self.assertEqual(originals, {key: (self.sdk / key).read_bytes() for key in SOURCES})

    def test_psram_storage_and_stacks(self):
        self.check_probe("patched", True)

    def test_unpatched_sdk_uses_sram(self):
        self.check_probe("unpatched", False)

    def test_source_signature_drift_fails_closed(self):
        source = self.sdk / "lwip_intf_v2_1/dhcpd/dhcp-server-main.c"
        source.write_bytes(source.read_bytes().replace(b"rtos_create_thread(&dhcpd_thread,",
                                                       b"rtos_create_thread (&dhcpd_thread,"))
        _, result = self.configure("drift", True)
        self.assertNotEqual(result.returncode, 0)
        diagnostic = result.stdout + result.stderr
        self.assertIn("expected 1", diagnostic)
        self.assertIn("anchors, found 0", diagnostic)

    def test_board_requires_psram_and_non_smp(self):
        _, result = self.configure("without-psram", True,
                                   ("-DCONFIG_PSRAM_AS_SYS_MEMORY=0",))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires non-SMP PSRAM heap", result.stdout + result.stderr)
        _, result = self.configure("smp", True, ("-DCONFIG_FREERTOS_SMP=1",))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires non-SMP PSRAM heap", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
