"""Compile real pinned SDK timeout callbacks after the production overlay.

Only SDK types and side effects are shimmed. The callback bodies and CMake
transformation are production inputs, including the separate disconnect after
lwIP's status callback. This is a host regression, not a board run.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--locator", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[5]
    sdk = Path(json.loads(args.locator.read_text())["paths"]["checkout"]
               if args.locator else os.environ["BK7258_PATH"])
    expected = (repo / "tools/bazel/native_versions/bk7258_sdk_commit.txt").read_text().strip()
    actual = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    assert actual == expected, (actual, expected)
    assert not subprocess.check_output(["git", "-C", str(sdk), "status", "--porcelain=v1", "--untracked-files=no"], text=True)
    source = sdk / "cp/components/bk_wifi/src/wifi_netif.c"
    overlay = repo / "native_component_src/bk7258/shared/h2_bk_wifi_ipv6.cmake"
    cmake, cc = shutil.which("cmake"), shutil.which("cc")
    assert cmake and cc, "cmake and a native C compiler are required"
    with tempfile.TemporaryDirectory(prefix="h2-bk-ipv6-timeout-") as temporary:
        work = Path(temporary)
        (work / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.16)
project(timeout C)
set(REPO_ROOT "{repo}")
set(H2_BK_CP_SDK_ROOT "{sdk / 'cp'}")
add_library(wifi OBJECT "{source}")
function(armino_component_get_property output component property)
  set(${{output}} wifi PARENT_SCOPE)
endfunction()
include("{overlay}")
get_target_property(result wifi SOURCES)
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/sources.txt" "${{result}}")
''')
        build = work / "on"
        subprocess.run([cmake, "-S", str(work), "-B", str(build), "-DCONFIG_IPV6=ON"], check=True, capture_output=True, timeout=60)
        generated = build / "h2_bk_cp_wifi_netif.c"
        assert (build / "sources.txt").read_text() == str(generated)
        transformed = generated.read_text()
        # Exact SDK bodies, including every side effect and injected guard.
        callbacks = transformed[transformed.index("void wifi_netif_notify_sta_dhcp_timeout(void)"):
                                transformed.index("bool wifi_netif_sta_is_connected(void)")]
        fixture = work / "callbacks.c"
        fixture.write_text('''#include <assert.h>
typedef struct { int state, reason_code; } wifi_linkstate_reason_t;
typedef struct { int netif_if; } netif_event_got_ip4_t;
enum { WIFI_LINKSTATE_STA_DISCONNECTED=1, WIFI_REASON_DHCP_TIMEOUT=260,
       NETIF_IF_STA=0, EVENT_MOD_NETIF=2, EVENT_NETIF_DHCP_TIMEOUT=3,
       BEKEN_NEVER_TIMEOUT=0 };
static int ready, status_calls, event_calls, disconnect_calls;
static int h2_bk_wifi_ipv6_ready(void) { return ready; }
static void mhdr_set_station_status(wifi_linkstate_reason_t info) {
  assert(info.reason_code == 260); ++status_calls;
}
static int bk_event_post(int module,int event,const void *data,unsigned size,int timeout) {
  (void)data;(void)size;(void)timeout;
  assert(module == EVENT_MOD_NETIF && event == EVENT_NETIF_DHCP_TIMEOUT);
  ++event_calls;return 0;
}
static void bk_wlan_dhcp_timeout(void) { ++disconnect_calls; }
#define WIFI_LOGD(...) ((void)0)
#define BK_LOG_ON_ERR(call) ((void)(call))
''' + callbacks + '''
int main(void) {
  ready=1;
  wifi_netif_notify_sta_dhcp_timeout();
  wifi_netif_notify_sta_disconnect();
  assert(status_calls==0 && event_calls==0 && disconnect_calls==0);
  ready=0;
  wifi_netif_notify_sta_dhcp_timeout();
  wifi_netif_notify_sta_disconnect();
  assert(status_calls==1 && event_calls==1 && disconnect_calls==1);
  return 0;
}
''')
        binary = work / "callbacks"
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(fixture), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=10)
        off = work / "off"
        subprocess.run([cmake, "-S", str(work), "-B", str(off), "-DCONFIG_IPV6=OFF"], check=True, capture_output=True, timeout=60)
        assert not (off / "h2_bk_cp_wifi_netif.c").exists()
        assert (off / "sources.txt").read_text() == str(source)
        print(json.dumps({"result": "PASS", "sdk_commit": actual,
                          "sdk_source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                          "overlay_sha256": hashlib.sha256(overlay.read_bytes()).hexdigest(),
                          "guarded_boundaries": ["DHCP timeout notification", "radio disconnect"],
                          "feature_off": "original SDK source"}, sort_keys=True))


if __name__ == "__main__":
    main()
