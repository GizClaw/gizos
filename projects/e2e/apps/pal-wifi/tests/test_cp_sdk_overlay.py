"""Execute the production paired-CP RPC correction; no hardware qualification."""
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
    raise RuntimeError("The SDK overlay regression requires the host CMake tool")


SDK = r'''
#include <stdint.h>
#include <stddef.h>
typedef int bk_err_t;
enum { BK_OK=0, BK_ERR_PARAM=-1, BK_ERR_NOT_FOUND=-2, BK_ERR_BUSY=-12, STA_PM_ENABLE=0x301, STA_STOP=0x312 };
struct bk_msg_hdr { uint32_t cmd_id; };
typedef struct __attribute__((packed)) { uint32_t argc; uintptr_t args[4]; } wifi_api_arg_info_t;
int started=1, connected=1, vif=7, calls=0, associate_calls=0, confirmed=999, failure=0;
int bk_wifi_sta_disconnect(void) {
    ++calls;
    if (failure) return failure;
    connected=0;
    return BK_OK;
}
int bk_wifi_sta_connect(void) {
    ++associate_calls;
    if (failure) return failure;
    if (!started) return -3;
    connected=1;
    return BK_OK;
}
int cif_bk_cmd_confirm(struct bk_msg_hdr *msg,uint8_t *value,size_t size) {
    (void)msg; (void)size; confirmed=*(int *)value; return BK_OK;
}
bk_err_t cif_handle_wifi_api_cmd(struct bk_msg_hdr *msg) {
    bk_err_t ret=BK_OK;
    wifi_api_arg_info_t *arg_info=(wifi_api_arg_info_t *)(msg+1);
    (void)arg_info;
    switch(msg->cmd_id) {
        case STA_PM_ENABLE:
            break;
        case STA_STOP:
            started=0; connected=0; vif=-1; break;
        default:
            ret=BK_ERR_NOT_FOUND; break;
    }
    cif_bk_cmd_confirm(msg,(uint8_t *)&ret,sizeof(ret));
    return BK_OK;
}
'''

PROBE = r'''
#include <stdint.h>
#include <stdio.h>
#include "h2_bk_wifi_rpc.h"
#include "h2_bk_dhcp_ring.h"
#include "h2_bk_wifi_lease.h"
struct bk_msg_hdr { uint32_t cmd_id; };
struct packet { struct bk_msg_hdr hdr; uint32_t argc; uintptr_t args[4]; };
extern int started,connected,vif,calls,associate_calls,confirmed,failure;
int cif_handle_wifi_api_cmd(struct bk_msg_hdr *);
int cif_send_customer_event(uint8_t *data, uint16_t len) { (void)data; (void)len; return 0; }
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"line%d\n",__LINE__); return 1;} } while(0)
int main(void) {
    struct packet p={{H2_BK_WIFI_RPC_STA_DISASSOCIATE},0,{0}};
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0);
#if EXPECT_RPC
    CHECK(confirmed==0 && !connected && started==1 && vif==7 && calls==1);
    p.hdr.cmd_id=H2_BK_WIFI_RPC_STA_ASSOCIATE;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0);
    CHECK(confirmed==0 && connected==1 && started==1 && vif==7 && associate_calls==1);
    p.argc=1;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0);
    CHECK(confirmed==-1 && associate_calls==1 && connected==1);
    p.argc=0; failure=-9; connected=0;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0);
    CHECK(confirmed==-9 && !connected && started==1 && vif==7);
    failure=0; started=0;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0);
    CHECK(confirmed==-3 && !connected);
    started=1; p.hdr.cmd_id=H2_BK_WIFI_RPC_STA_DISASSOCIATE;
    connected=1; p.argc=1;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0);
    CHECK(confirmed==-1 && connected==1 && started==1 && vif==7 && calls==1);
    p.argc=0; failure=-9;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0);
    CHECK(confirmed==-9 && connected==1 && started==1 && vif==7 && calls==2);
    p.hdr.cmd_id=H2_BK_WIFI_RPC_DHCP_SNAPSHOT; p.argc=0;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0 && confirmed==-1);
    p.argc=1; p.args[0]=0;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0 && confirmed==-1);
    h2_bk_dhcp_snapshot_t snapshot;
    h2_bk_dhcp_entry_t entry={.xid=123,.dir=1,.type=3};
    h2_bk_dhcp_record(&entry);
    p.args[0]=(uintptr_t)&snapshot;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0 && confirmed==0);
    CHECK(snapshot.version==H2_BK_DHCP_SNAPSHOT_VERSION && snapshot.count==1 && snapshot.entries[0].xid==123);
    p.hdr.cmd_id=H2_BK_WIFI_RPC_LEASE_SNAPSHOT; p.argc=0;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0 && confirmed==-1);
    p.argc=1; p.args[0]=0;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0 && confirmed==-1);
    const uint8_t mac[6]={0x30,0xed,0xa0,0xae,0x0f,0x84};
    h2_bk_wifi_lease_reset();
    CHECK(h2_bk_wifi_lease_accept(mac,0xc0a8bc64u,567u)==1u);
    h2_bk_wifi_lease_snapshot_t leases;
    p.args[0]=(uintptr_t)&leases;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0 && confirmed==0);
    CHECK(leases.version==H2_BK_WIFI_LEASE_VERSION && leases.count==1 &&
          leases.records[0].ip4==0xc0a8bc64u && leases.records[0].xid==567u);
#else
    CHECK(confirmed==-2 && connected==1 && started==1 && vif==7 && calls==0);
    p.hdr.cmd_id=H2_BK_WIFI_RPC_STA_ASSOCIATE;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0);
    CHECK(confirmed==-2 && connected==1 && started==1 && vif==7 && associate_calls==0);
    p.hdr.cmd_id=H2_BK_WIFI_RPC_LEASE_SNAPSHOT;
    CHECK(cif_handle_wifi_api_cmd(&p.hdr)==0 && confirmed==-2);
#endif
    puts("CP_RPC_CONFIRMED_PAYLOAD_AND_SERVICE_PRESERVATION PASS");
    return 0;
}
'''


class PairedRPC(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        self.repo=Path.cwd()
        directory=self.root/'sdk/components/controller_if'
        directory.mkdir(parents=True)
        self.sdk=directory/'cif_wifi_api.c'
        self.sdk.write_bytes(SDK.replace('\n','\r\n').encode())
        (self.root/'probe.c').write_text(PROBE)
        production=Path('native_component_src/bk7258/cp/h2_pal_core/CMakeLists.txt').read_text()
        (self.root/'rpc.cmake').write_text(production[production.index('# The public AP SDK disconnect'):])
        (self.root/'CMakeLists.txt').write_text('''
cmake_minimum_required(VERSION 3.16)
project(PairedRPC C)
set(CMAKE_C_STANDARD 11)
set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
set(H2_BK_CP_PSRAM_SERVICES OFF)
set(H2_BK_CP_LEASE_HOOK OFF)
add_library(sdk STATIC sdk/components/controller_if/cif_wifi_api.c
 "${REPO_ROOT}/native_component_src/bk7258/cp/h2_pal_core/src/h2_bk_dhcp_ring.c"
 "${REPO_ROOT}/native_component_src/bk7258/cp/h2_pal_core/src/h2_bk_wifi_lease.c")
target_compile_definitions(sdk PRIVATE H2_BK_WIFI_LEASE_TEST=1)
target_include_directories(sdk PRIVATE "${REPO_ROOT}/native_component_src/bk7258/shared")
function(armino_component_get_property out component property)
  set(${out} sdk PARENT_SCOPE)
endfunction()
if(APPLY_RPC)
 include("${CMAKE_CURRENT_SOURCE_DIR}/rpc.cmake")
endif()
add_executable(probe probe.c)
target_include_directories(probe PRIVATE "${REPO_ROOT}/native_component_src/bk7258/shared")
target_compile_definitions(probe PRIVATE EXPECT_RPC=$<BOOL:${APPLY_RPC}>)
target_link_libraries(probe PRIVATE sdk)
''')

    def configure(self,name,apply):
        build=self.root/name
        env=dict(os.environ,H2_GIZOS_ROOT=str(self.repo),ARMINO_PATH=str(self.root/'sdk'))
        result=subprocess.run([cmake_tool(),'-S',str(self.root),'-B',str(build),
            '-DAPPLY_RPC='+('ON' if apply else 'OFF')],env=env,capture_output=True,text=True)
        return build,result

    def check_probe(self,name,apply):
        before=self.sdk.read_bytes()
        build,r=self.configure(name,apply)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        r=subprocess.run([cmake_tool(),'--build',str(build),'--config','Debug'],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        probe=next(path for path in (build/'probe',build/'probe.exe',build/'Debug/probe.exe') if path.is_file())
        r=subprocess.run([str(probe)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('CP_RPC_CONFIRMED_PAYLOAD_AND_SERVICE_PRESERVATION PASS',r.stdout)
        self.assertEqual(self.sdk.read_bytes(),before,'sealed SDK modified')

    def test_corrected_rpc_and_invalid_arg_and_real_failure_payload(self):
        self.check_probe('corrected',True)

    def test_old_cp_reports_unknown_even_when_transport_succeeds(self):
        self.check_probe('old',False)

    def test_anchor_drift_fails_configure(self):
        self.sdk.write_bytes(self.sdk.read_bytes().replace(b'case STA_PM_ENABLE:',b'case 0x301:'))
        _,r=self.configure('drift',True)
        self.assertNotEqual(r.returncode,0)
        self.assertIn('Pinned BK CP API dispatch changed',r.stdout+r.stderr)


if __name__=='__main__':
    unittest.main()
