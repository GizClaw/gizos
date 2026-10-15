"""Execute the actual BK test run sequence and pre-ACK serial log demux.

SDK/time boundaries are scripted here; this is lifecycle regression, never a
physical-board qualification receipt or a replacement for the c93 oracle.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[5]


def section(text, begin, end):
    start = text.index(begin)
    return text[start:text.index(end, start)]


def compile_run(source, includes=(), args=()):
    with tempfile.TemporaryDirectory(prefix="pref-console-contract-") as directory:
        unit = Path(directory) / "contract.c"
        binary = Path(directory) / "contract"
        unit.write_text(source)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        *["-I" + str(ROOT / folder) for folder in includes],
                        str(unit), "-o", str(binary)], check=True)
        return subprocess.run([str(binary), *args], check=True, text=True,
                              capture_output=True, timeout=10).stdout


class BKConsoleLifecycle(unittest.TestCase):
    def test_run_control_ownership_and_outcomes(self):
        launcher = (ROOT / "projects/e2e/targets/h2loader_tar_zlib/pal-storage/bk7258_v3_202405/ap/ap_main.c").read_text()
        run = section(launcher, "static void run(", "static void entry(")
        fixture = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define H2_PAL_OK 0
#define H2_PAL_ERR_INVALID_STATE -7
#define H2_LOADER_CAPABILITY_UART 1
#define H2_STORAGE_VERSION "console-test"
typedef struct { int unused; } h2_runtime_t;
static h2_runtime_t actual;
static h2_runtime_t *runtime=&actual;
static volatile int finished;
static int suite_rc,stop_rc,restart_rc,confirm_rc;
static int active=1,stops,starts,runs,replays,confirms,replay_after,settle,drain,repeats;
static jmp_buf done;
static void rtos_delay_milliseconds(unsigned ms) {
    if(ms==60000u){assert(!stops && !runs);settle++;}
    else if(ms==200u){assert(!active && runs==1 && replays==1);drain++;}
    else {assert(ms==3000u && active && finished);if(++repeats==3)longjmp(done,1);}
}
static int h2_bk_h2loader_stop_app_iostreamikcp(void) {
    assert(settle==1 && !runs);stops++;if(!stop_rc)active=0;return stop_rc;
}
static int h2_storage_device_run(h2_runtime_t *r,const char *path,const char *version) {
    assert(r==runtime && !active && !strcmp(path,"/data/pal-storage") && !strcmp(version,H2_STORAGE_VERSION));
    runs++;puts("TEST_NATIVE_FRESH emitted_once");return suite_rc;
}
static void h2_storage_device_replay(h2_runtime_t *r) {
    assert(r==runtime && runs==1);replays++;
    if(active){assert(starts==1);replay_after++;}else{assert(!starts);}
    puts("TEST_IMMUTABLE_REPLAY");
}
static int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(h2_runtime_t *r,const char *name,int capability) {
    assert(r==runtime && !strcmp(name,"pal-storage") && capability==1 && replays==1 && drain==1);starts++;if(!restart_rc)active=1;return restart_rc;
}
static int h2_bk_h2loader_confirm_current_app(h2_runtime_t *r) {
    assert(r==runtime && active && starts==1 && suite_rc==0);confirms++;return confirm_rc;
}
/* RUN */
int main(int argc,char **argv){
    assert(argc==2);int scenario=atoi(argv[1]);
    if(scenario==1)suite_rc=-4;
    if(scenario==2)stop_rc=-4;
    if(scenario==3)restart_rc=-4;
    if(scenario==4)confirm_rc=-4;
    if(!setjmp(done))run(NULL);
    assert(finished && settle==1 && stops==1);
    if(scenario==2){assert(!runs && !replays && !starts && !confirms);}
    else if(scenario==3){assert(runs==1 && replays==1 && starts==1 && !confirms);}
    else {assert(runs==1 && replays==3 && starts==1 && replay_after==2);assert(confirms==(scenario==1?0:1));}
    return 0;
}
'''.replace("/* RUN */", run)
        for scenario in range(5):
            with self.subTest(scenario=scenario):
                output = compile_run(fixture, args=[str(scenario)])
                self.assertEqual(output.count("TEST_NATIVE_FRESH"), 0 if scenario == 2 else 1)
                if scenario == 0:
                    self.assertEqual(output.count("H2_STORAGE_READY rc=0 confirm=0"), 3)
                    self.assertLess(output.index("TEST_IMMUTABLE_REPLAY"), output.index("H2_STORAGE_CONTROL_RESTORED"))
                else:
                    self.assertNotIn("H2_STORAGE_READY rc=0 confirm=0", output)

    def test_real_filter_forwards_fresh_boot_before_session_ack(self):
        serial = (ROOT / "libs/h2loader_host/src/h2_h2loader_host_serial.c").read_text()
        handshake = section(serial, "typedef struct session_ack_context", "/* A USB-UART adapter survives")
        frame = (ROOT / "libs/iostreamikcp/src/h2_iostreamikcp_frame.c").read_text().replace(
            '#include "h2_iostreamikcp_internal.h"', '#include "h2_iostreamikcp.h"\n#define H2_IOSTREAMIKCP_FRAME_MAGIC_LEN 6u\n#define H2_IOSTREAMIKCP_FRAME_LEN_OFFSET 12u')
        fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "h2_h2loader_host.h"
#include "h2_iostreamikcp.h"
#define H2_H2LOADER_HOST_SERIAL_POLL_MS 10u
#define H2_H2LOADER_HOST_SERIAL_SESSION_RETRY_MS 200u
struct h2_h2loader_host_serial_connection {
 const h2_pal_time_api_t *time;const h2_pal_uart_io_stream_api_t *uart;
 h2_h2loader_host_transport_log_fn on_log;void *log_user;
 uint8_t pending_input[512];size_t pending_input_len;
};
/* FRAME */
/* HANDSHAKE */
static uint64_t now;
static uint8_t wire[512];static size_t wire_size,position,limit;
static char received[512];static size_t logged;
static h2_pal_result_t monotonic(void *u,uint64_t *out){(void)u;*out=now;return 0;}
static const h2_pal_time_vtable_t time_methods={.get_monotonic_ms=monotonic};
static const h2_pal_time_api_t clock_api={.vtable=&time_methods};
static h2_pal_result_t read_wire(void *u,void *out,size_t capacity,size_t *length,uint32_t wait){
 (void)u;now+=wait;size_t n=wire_size-position;if(n>capacity)n=capacity;if(n>limit)n=limit;
 memcpy(out,wire+position,n);position+=n;*length=n;return n?0:H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t write_wire(void *u,const void *data,size_t n,size_t *out,uint32_t wait){(void)u;(void)data;(void)wait;*out=n;return 0;}
static h2_pal_result_t flush_wire(void *u){(void)u;return 0;}
static const h2_pal_uart_io_stream_vtable_t uart_methods={.read=read_wire,.write=write_wire,.flush=flush_wire};
static const h2_pal_uart_io_stream_api_t uart_api={.vtable=&uart_methods};
static h2_pal_result_t sink(void *u,const uint8_t *data,size_t n){(void)u;assert(logged+n<sizeof(received));memcpy(received+logged,data,n);logged+=n;received[logged]=0;return 0;}
int main(void){
 (void)serial_stream_now_ms;
 const char fresh[]="H2_STORAGE_BOOT contract=2 version=wire-test phase=1 nonce=17\n";
 for(int ack=0;ack<2;++ack)for(size_t chunk=1;chunk<=128;chunk*=2){
   now=0;position=0;logged=0;limit=chunk;memcpy(wire,fresh,sizeof(fresh)-1);wire_size=sizeof(fresh)-1;
   if(ack){const uint8_t conv[]={7,0,0,0};h2_iostreamikcp_frame_t f={.flags=H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK,.conv=7,.payload=conv,.payload_len=4};size_t n=0;
     assert(!h2_iostreamikcp_frame_encode(&f,wire+wire_size,sizeof(wire)-wire_size,&n));wire_size+=n;}
   h2_h2loader_host_serial_connection_t connection={.time=&clock_api,.uart=&uart_api,.on_log=sink};
   int rc=serial_session_control(&connection,7,H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN,2000);
   assert(rc==(ack?H2_PAL_OK:H2_PAL_ERR_TIMEOUT));assert(logged==sizeof(fresh)-1 && !memcmp(received,fresh,logged));
 }
 puts("PASS native fresh BOOT reaches sink with fragmented/no/delayed session ACK");return 0;
}
'''.replace("/* FRAME */", frame).replace("/* HANDSHAKE */", handshake)
        output = compile_run(fixture, includes=["libs/pal/include", "libs/h2loader_host/include", "libs/iostreamikcp/include"])
        self.assertIn("PASS native fresh BOOT reaches sink", output)


if __name__ == "__main__":
    unittest.main()
