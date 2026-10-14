"""Execute the SDK shutdown implementations with faulting native boundaries."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[5]
ESP_TRANSPORT = ROOT / "projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_runtime/src/h2_esp_h2loader_iostreamikcp.c"
ESP_OWNER = ROOT / "projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_ble/src/h2_esp_h2loader_ble.c"
BK_OWNER = ROOT / "projects/h2loader/native_component_src/bk7258/ap/h2loader_bk/src/h2_bk_h2loader_app_bleikcp.c"

def function(source, name):
    match = re.search(r"(?m)^(?:static\s+)?(?:int|h2_pal_result_t)\s+" + name + r"\s*\(", source)
    if match is None:
        raise ValueError(name)
    brace = source.index("{", match.start())
    depth = 0
    for token in re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]', source[brace:]):
        if token.group() == "{": depth += 1
        if token.group() == "}":
            depth -= 1
            if depth == 0: return source[match.start():brace + token.end()]
    raise ValueError(name)

TRANSPORT = r"""
#include "h2_atomic.h"
#include "h2/pal/core/h2_pal_errors.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define H2_LOADER_APP_CLIENT_SESSION_RESET -100
struct transport { int close_pending, pending_conv, replacement_pending; };
typedef struct transport h2_esp_h2loader_command_transport_t;
typedef struct { int cookie; } h2_loader_app_client_t;
struct io_vtable {
 int (*read)(void*,void*,size_t,size_t*,uint32_t);
 int (*write)(void*,const void*,size_t,size_t*,uint32_t);
 int (*flush)(void*);
};
struct io { void *user; const struct io_vtable *vtable; };
typedef struct {
 h2_esp_h2loader_command_transport_t transport;
 struct io io;
 h2_loader_app_client_t *client;
 h2_atomic_bool_t stop_requested;
} h2_esp_h2loader_app_iostreamikcp_t;
static h2_esp_h2loader_app_iostreamikcp_t s_app_transport;
static int s_app_transport_started, join_result, joins, deinit_calls, io_calls;
static void h2_esp_h2loader_command_transport_deactivate(struct transport *t) {(void)t; ++io_calls;}
static int h2_esp_h2loader_command_transport_activate_pending(struct transport *t) {(void)t; ++io_calls; return H2_PAL_OK;}
static int h2_esp_h2loader_command_transport_has_session(struct transport *t) {(void)t; return 0;}
static int h2_esp_h2loader_command_transport_poll_session(struct transport *t,uint32_t ms) {(void)t;(void)ms; ++io_calls; return H2_PAL_ERR_TIMEOUT;}
static int h2_esp_h2loader_command_transport_replacement_pending(struct transport *t) {(void)t; return 0;}
static int h2_loader_app_client_stop_return_console(h2_loader_app_client_t *c) {
 assert(c && c->cookie == 123);
 assert(h2_atomic_bool_load(&s_app_transport.stop_requested,H2_ATOMIC_ACQUIRE));
 ++joins; return join_result;
}
static void h2_esp_h2loader_command_transport_deinit(struct transport *t) {
 assert(t == &s_app_transport.transport && joins > 0 && join_result == H2_PAL_OK);
 ++deinit_calls;
}
static int read_io(void *u,void *p,size_t n,size_t *o,uint32_t m) {(void)u;(void)p;(void)n;(void)m;*o=0;++io_calls;return H2_PAL_ERR_TIMEOUT;}
static int write_io(void *u,const void *p,size_t n,size_t *o,uint32_t m) {(void)u;(void)p;(void)m;*o=n;++io_calls;return H2_PAL_OK;}
static int flush_io(void *u) {(void)u;++io_calls;return H2_PAL_OK;}
static const struct io_vtable io = {read_io,write_io,flush_io};
/* FUNCTIONS */
int main(void) {
 h2_loader_app_client_t client = {123};
 assert(h2_esp_h2loader_app_iostreamikcp_stop() == H2_PAL_OK && joins == 0);
 s_app_transport_started=1;s_app_transport.client=&client;s_app_transport.io.vtable=&io;
 assert(h2_atomic_bool_init(&s_app_transport.stop_requested,false)==H2_ATOMIC_OK);
 join_result=H2_PAL_ERR_TIMEOUT;
 assert(h2_esp_h2loader_app_iostreamikcp_stop()==H2_PAL_ERR_TIMEOUT);
 assert(s_app_transport_started && s_app_transport.client==&client && deinit_calls==0);
 assert(s_app_transport.stop_requested.storage!=NULL);
 assert(app_read_byte(&s_app_transport,10)==EOF && io_calls==0);
 assert(app_write(&s_app_transport,"packet",6)==H2_PAL_ERR_CLOSED && io_calls==0);
 join_result=H2_PAL_OK;
 assert(h2_esp_h2loader_app_iostreamikcp_stop()==H2_PAL_OK);
 assert(joins==2 && deinit_calls==1 && !s_app_transport_started);
 assert(s_app_transport.client==NULL && s_app_transport.stop_requested.storage==NULL);
 assert(h2_esp_h2loader_app_iostreamikcp_stop()==H2_PAL_OK && joins==2 && deinit_calls==1);
 return 0;
}
"""
OWNER = r"""
#include "h2/pal/core/h2_pal_errors.h"
#include <assert.h>
#include <string.h>
typedef struct { int cookie; } h2_pal_mutex_t;
typedef struct { int cookie; } h2_pal_sync_api_t;
typedef struct { const h2_pal_sync_api_t *sync; } h2_runtime_t;
static h2_pal_sync_api_t sync = {123};
static h2_pal_mutex_t mutex = {456};
static h2_runtime_t runtime = {&sync};
static struct {
 void *service;
 h2_pal_mutex_t *operation_mutex;
 const h2_pal_sync_api_t *operation_sync;
 h2_runtime_t *runtime;
} s_ble;
static struct { int marker; } s_serial_client;
static int s_serial_started, transport_error, mutex_error, stops, destroyed;
static int stop_transport(void) {++stops;return transport_error;}
#define h2_esp_h2loader_app_iostreamikcp_stop stop_transport
#define h2_bk_h2loader_stop_app_iostreamikcp stop_transport
static int h2_pal_mutex_destroy(const h2_pal_sync_api_t *api,h2_pal_mutex_t *m) {
 assert(api==&sync && m==&mutex && transport_error==H2_PAL_OK && stops>0);
 ++destroyed;return mutex_error;
}
/* FUNCTION */
int main(void) {
 s_ble.service=&runtime;s_ble.operation_mutex=&mutex;s_ble.operation_sync=&sync;s_ble.runtime=&runtime;s_serial_started=1;
 assert(STOP_OWNER()==H2_PAL_ERR_BUSY && stops==0 && destroyed==0);
 s_ble.service=NULL;transport_error=H2_PAL_ERR_TIMEOUT;
 assert(STOP_OWNER()==H2_PAL_ERR_TIMEOUT && destroyed==0 && s_ble.operation_mutex==&mutex);
 transport_error=H2_PAL_OK;mutex_error=H2_PAL_ERR_BUSY;
 assert(STOP_OWNER()==H2_PAL_ERR_BUSY && s_ble.operation_mutex==&mutex && s_ble.runtime==&runtime);
 mutex_error=H2_PAL_OK;
 assert(STOP_OWNER()==H2_PAL_OK && s_ble.operation_mutex==NULL && s_ble.runtime==NULL && s_ble.operation_sync==NULL);
 assert(STOP_OWNER()==H2_PAL_OK && destroyed==2);
 return 0;
}
"""

class NativeShutdown(unittest.TestCase):
    def compile_and_run(self, text, atomic=False):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/"test.c";binary=Path(tmp)/"test";source.write_text(text)
            command=["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-unused-variable",
                "-I"+str(ROOT/"libs/pal/include"),"-I"+str(ROOT/"libs/atomic/include"),str(source)]
            if atomic:command.append(str(ROOT/"libs/atomic/providers/c11/src/h2_atomic_c11.c"))
            compiled = subprocess.run(command+["-o",str(binary)],capture_output=True,text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(binary)],check=True,capture_output=True,text=True,timeout=10)

    def test_esp_transport_rejected_join_keeps_borrows_and_closes_admission(self):
        text=ESP_TRANSPORT.read_text()
        functions="\n".join(function(text,name) for name in ("app_read_byte","app_write","h2_esp_h2loader_app_iostreamikcp_stop"))
        self.compile_and_run(TRANSPORT.replace("/* FUNCTIONS */",functions),atomic=True)

    def test_owner_drain_before_mutex_and_retry(self):
        for path,name in [(ESP_OWNER,"h2_esp_h2loader_app_commands_stop_serial"),(BK_OWNER,"h2_bk_h2loader_stop_app_commands")]:
            with self.subTest(sdk=str(path)):
                self.compile_and_run(OWNER.replace("/* FUNCTION */",function(path.read_text(),name)).replace("STOP_OWNER()",name+"()"))

if __name__=="__main__":unittest.main()
