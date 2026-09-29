"""Execute the production CMake SDK correction against a minimal broken SDK."""
import hashlib
import os
from pathlib import Path
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


HEADER = r'''
#ifndef STUB_H
#define STUB_H
#include <stddef.h>
typedef int bk_err_t;
typedef enum { NETIF_IF_STA=0, NETIF_IF_AP=1 } netif_if_t;
typedef struct { char ip[16], mask[16], gateway[16], dns[16]; } netif_ip4_config_t;
enum { BK_OK=0, BK_ERR_NULL_PARAM=-1 };
extern int ipc_calls, sta_updates, ap_updates;
extern netif_ip4_config_t cp_ap, local_sta;
void ip_address_set(int iface,int dhcp,char *ip,char *mask,char *gw,char *dns);
bk_err_t bk_netif_set_ip4_config(netif_if_t,const netif_ip4_config_t *);
bk_err_t sdk_internal_sta_callback(const netif_ip4_config_t *);
int sdk_other_source(void);
#endif
'''

BROKEN_SDK = r'''
#include "stub.h"
#include <string.h>
int ipc_calls, sta_updates, ap_updates;
netif_ip4_config_t cp_ap, local_sta;
void ip_address_set(int iface,int dhcp,char *ip,char *mask,char *gw,char *dns) {
    (void)dhcp; (void)mask; (void)gw; (void)dns;
    if (iface==1) { ++sta_updates; strcpy(local_sta.ip,ip); }
    else ++ap_updates;
}
bk_err_t bk_netif_set_ip4_config(netif_if_t ifx, const netif_ip4_config_t *ip4_config)
{
    if (!ip4_config) return BK_ERR_NULL_PARAM;
    ++ipc_calls;
    cp_ap=*ip4_config;
    netif_ip4_config_t mutable_config=*ip4_config;
    ip_address_set(ifx==NETIF_IF_STA,0,mutable_config.ip,mutable_config.mask,mutable_config.gateway,mutable_config.dns);
    return BK_OK;
}
bk_err_t sdk_internal_sta_callback(const netif_ip4_config_t *config) {
    return bk_netif_set_ip4_config(NETIF_IF_STA,config);
}
'''

PROBE = r'''
#include "stub.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failed line %d\n",__LINE__); return 1; } } while(0)
int main(void) {
    netif_ip4_config_t ap={.ip="192.168.188.1",.mask="255.255.255.0"};
    netif_ip4_config_t first={.ip="192.168.4.2"},second={.ip="192.168.4.3"};
    CHECK(sdk_other_source()==77);
    CHECK(bk_netif_set_ip4_config(NETIF_IF_AP,&ap)==BK_OK);
    CHECK(ipc_calls==1 && ap_updates==1 && !strcmp(cp_ap.ip,ap.ip));
    CHECK(sdk_internal_sta_callback(&first)==BK_OK);
    CHECK(sta_updates==1 && !strcmp(local_sta.ip,first.ip));
    CHECK(ipc_calls==1 && !strcmp(cp_ap.ip,ap.ip));
    CHECK(bk_netif_set_ip4_config(NETIF_IF_STA,&second)==BK_OK);
    CHECK(sta_updates==2 && !strcmp(local_sta.ip,second.ip));
    CHECK(ipc_calls==1 && !strcmp(cp_ap.ip,ap.ip));
    CHECK(bk_netif_set_ip4_config(NETIF_IF_STA,NULL)==BK_ERR_NULL_PARAM);
    CHECK(sta_updates==2 && ipc_calls==1);
    strcpy(ap.ip,"192.168.188.2");
    CHECK(bk_netif_set_ip4_config(NETIF_IF_AP,&ap)==BK_OK);
    CHECK(ipc_calls==2 && ap_updates==2 && !strcmp(cp_ap.ip,ap.ip));
    puts("SDK_OVERLAY_INTERNAL_EXTERNAL_STA_AND_AP PASS");
    return 0;
}
'''


class SDKCorrection(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        sdk=self.root/'sdk/components/bk_netif'
        sdk.mkdir(parents=True)
        self.sdk_source=sdk/'bk_netif.c'
        self.sdk_source.write_text(BROKEN_SDK)
        (self.root/'stub.h').write_text(HEADER)
        (self.root/'extra.c').write_text('int sdk_other_source(void) {return 77;}\n')
        (self.root/'probe.c').write_text(PROBE)
        production=Path('native_component_src/bk7258/ap/h2_pal_core/CMakeLists.txt').read_text()
        start=production.index('# The pinned AP SDK erroneously')
        end=production.index('# The pinned AP SDK swallows', start)
        (self.root/'correction.cmake').write_text(production[start:end])
        (self.root/'CMakeLists.txt').write_text('''
cmake_minimum_required(VERSION 3.16)
project(SDKCorrection C)
set(CMAKE_C_STANDARD 11)
add_library(sdk STATIC sdk/components/bk_netif/bk_netif.c extra.c)
target_include_directories(sdk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set(BK_ARMINO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
function(armino_component_get_property out component property)
  set(${out} sdk PARENT_SCOPE)
endfunction()
if(APPLY_CORRECTION)
  include("${CMAKE_CURRENT_SOURCE_DIR}/correction.cmake")
endif()
add_executable(probe probe.c)
target_link_libraries(probe PRIVATE sdk)
''')

    def configure(self,name,apply=True):
        build=self.root/name
        return build,subprocess.run([cmake_tool(),'-S',str(self.root),'-B',str(build),
            '-DAPPLY_CORRECTION='+('ON' if apply else 'OFF')],capture_output=True,text=True)

    def run_probe(self,name,apply=True):
        build,result=self.configure(name,apply)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        result=subprocess.run([cmake_tool(),'--build',str(build),'--config','Debug'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        probe=next(path for path in (build/'probe',build/'probe.exe',build/'Debug/probe.exe') if path.is_file())
        return subprocess.run([str(probe)],capture_output=True,text=True)

    def test_production_generated_copy_repairs_same_tu_and_external_calls(self):
        original=self.sdk_source.read_bytes()
        result=self.run_probe('fixed')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('SDK_OVERLAY_INTERNAL_EXTERNAL_STA_AND_AP PASS',result.stdout)
        self.assertEqual(self.sdk_source.read_bytes(),original,'SDK source was modified')

    def test_original_actual_bug_fails_identical_behavior_probe(self):
        result=self.run_probe('original',apply=False)
        self.assertNotEqual(result.returncode,0,'Original SDK unexpectedly preserved CP AP')

    def test_sdk_signature_drift_fails_configure(self):
        self.sdk_source.write_text(BROKEN_SDK.replace('netif_if_t ifx, const','netif_if_t interface_id, const'))
        _,result=self.configure('drift')
        self.assertNotEqual(result.returncode,0)
        self.assertIn('Pinned BK Netif STA sync signature changed',result.stdout+result.stderr)


if __name__=='__main__':
    unittest.main()
