"""Compile the pinned AP SDK stop correction, including its failure path."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


def cmake_tool():
    candidates = [os.environ.get("CMAKE"), shutil.which("cmake"),
                  "/opt/homebrew/bin/cmake", "/usr/local/bin/cmake",
                  "C:/Program Files/CMake/bin/cmake.exe"]
    for path in candidates:
        if path and Path(path).is_file():
            return path
    raise RuntimeError("The SDK overlay regression requires CMake")


SDK = r'''
#include <stdint.h>
typedef int bk_err_t;
enum { BK_OK=0, BK_ERR_NO_MEM=-4101, AP_STOP=0x510, WIFI_AP_STARTED_BIT=1 };
int cp_ret, cp_calls, local_ip_down, netif_removed, state_cleared, started=1;
#define WDRV_LOGD(...) ((void)0)
#define WDRV_LOGW(...) ((void)0)
#define WDRV_LOGI(...) ((void)0)
int wifi_ap_is_started(void) { return started; }
int wifi_send_com_api_cmd(int cmd, int argc) {
    if (cmd!=AP_STOP || argc!=0) return -1;
    ++cp_calls;
    return cp_ret;
}
void uap_ip_down(void) { ++local_ip_down; }
void host_wlan_remove_sap_netif(void) { ++netif_removed; }
void wifi_clear_state_bit(int bit) { if(bit==WIFI_AP_STARTED_BIT) ++state_cleared; }
bk_err_t bk_wifi_ap_stop(void)
{
    bk_err_t ret = BK_OK;

    if (!wifi_ap_is_started()) {
        WDRV_LOGD("ap stop: already stopped\n");
        return BK_OK;
    }

    ret = wifi_send_com_api_cmd(AP_STOP, 0);
    if (ret != BK_OK) {
        WDRV_LOGW("ap stop fail ret %d\n", ret);
    }

    WDRV_LOGI("ap stopped\n");
    uap_ip_down();
    host_wlan_remove_sap_netif();
    wifi_clear_state_bit(WIFI_AP_STARTED_BIT);

    return BK_OK;
}
'''

PROBE = r'''
#include <stdio.h>
extern int cp_ret, cp_calls, local_ip_down, netif_removed, state_cleared, started;
int bk_wifi_ap_stop(void);
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"failed line %d\n",__LINE__); return 1;} } while(0)
int main(void) {
    cp_ret=-4101;
    CHECK(bk_wifi_ap_stop()==-4101);
    CHECK(cp_calls==1 && !local_ip_down && !netif_removed && !state_cleared);
    cp_ret=0;
    CHECK(bk_wifi_ap_stop()==0);
    CHECK(cp_calls==2 && local_ip_down==1 && netif_removed==1 && state_cleared==1);
    started=0;
    CHECK(bk_wifi_ap_stop()==0 && cp_calls==2);
    puts("AP_STOP_CP_FAILURE_PRESERVES_LOCAL_ADAPTER PASS");
    return 0;
}
'''


class APStopCorrection(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        sdk = self.root / "sdk/components/bk_wifi/src"
        sdk.mkdir(parents=True)
        self.source = sdk / "wifi_api.c"
        self.source.write_text(SDK)
        (self.root / "probe.c").write_text(PROBE)
        production = Path("native_component_src/bk7258/ap/h2_pal_core/CMakeLists.txt").read_text()
        start = production.index("# The pinned AP SDK swallows")
        (self.root / "correction.cmake").write_text(production[start:])
        (self.root / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.16)
project(APStopCorrection C)
set(CMAKE_C_STANDARD 11)
set(BK_ARMINO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
add_library(sdk STATIC sdk/components/bk_wifi/src/wifi_api.c)
function(armino_component_get_property out component property)
  set(${out} sdk PARENT_SCOPE)
endfunction()
if(APPLY_CORRECTION)
  include("${CMAKE_CURRENT_SOURCE_DIR}/correction.cmake")
endif()
add_executable(probe probe.c)
target_link_libraries(probe PRIVATE sdk)
''')

    def configure(self, name, apply=True):
        build = self.root / name
        result = subprocess.run([cmake_tool(), "-S", str(self.root), "-B", str(build),
                                 "-DAPPLY_CORRECTION=" + ("ON" if apply else "OFF")],
                                capture_output=True, text=True)
        return build, result

    def run_probe(self, name, apply=True):
        build, result = self.configure(name, apply)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([cmake_tool(), "--build", str(build), "--config", "Debug"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        probe = next(path for path in (build / "probe", build / "probe.exe",
                                     build / "Debug/probe.exe") if path.is_file())
        return subprocess.run([str(probe)], capture_output=True, text=True)

    def test_cp_failure_is_returned_without_local_teardown(self):
        old = self.source.read_bytes()
        result = self.run_probe("corrected")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("AP_STOP_CP_FAILURE_PRESERVES_LOCAL_ADAPTER PASS", result.stdout)
        self.assertEqual(self.source.read_bytes(), old, "sealed SDK was modified")

    def test_original_sdk_falsely_succeeds_and_removes_local_adapter(self):
        result = self.run_probe("original", apply=False)
        self.assertNotEqual(result.returncode, 0)

    def test_sdk_anchor_drift_fails_configure(self):
        self.source.write_text(SDK.replace("wifi_send_com_api_cmd(AP_STOP, 0)",
                                           "wifi_send_com_api_cmd(AP_STOP, 1)"))
        _, result = self.configure("drift")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Pinned BK AP stop RPC signature changed", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
