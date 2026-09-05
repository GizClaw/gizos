"""Execute the board's connection-complete path with ATT failures injected."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c"


class BleConnectionTest(unittest.TestCase):
    def test_mac_failure_prevents_stack_start_and_allows_retry(self):
        source = SOURCE.read_text()
        begin = source.index("static int h2_ble_start(")
        end = source.index("static int h2_ble_stop(", begin)
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
enum { H2_PAL_OK=0, H2_PAL_ERR_IO=-1 };
const uint64_t config_btctler_le_features=0;
static struct { int started, starting; } h2_ble;
static int mac_result, stack_result, stack_calls, mac_calls;
void lmp_set_sniff_disable(void) {}
static const uint8_t *h2_ble_base_mac(void) {
 static const uint8_t address[6]={1,2,3,4,5,6}; return address;
}
void lib_make_ble_address(uint8_t *out, uint8_t *in) { memcpy(out,in,6); }
int le_controller_set_mac(void *addr) {
 assert(memcmp(addr,h2_ble_base_mac(),6)==0); ++mac_calls; return mac_result;
}
static int btstack_init(void) { ++stack_calls; return stack_result; }
'''
        main = r'''
int main(void) {
 const int failures[]={-1,1,-100,127};
 for (unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);++i) {
  mac_result=failures[i];
  assert(h2_ble_start(NULL)==H2_PAL_ERR_IO);
  assert(!h2_ble.starting && !h2_ble.started && stack_calls==0);
 }
 assert(mac_calls==4);
 mac_result=0; stack_result=-1;
 assert(h2_ble_start(NULL)==H2_PAL_ERR_IO && !h2_ble.starting);
 stack_result=0;
 assert(h2_ble_start(NULL)==0 && h2_ble.starting && stack_calls==2);
 assert(h2_ble_start(NULL)==0 && stack_calls==2 && mac_calls==6);
 h2_ble.starting=0; h2_ble.started=1;
 assert(h2_ble_start(NULL)==0 && stack_calls==2 && mac_calls==6);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-ble-start-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + source[begin:end] + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_notify_validates_length_before_narrowing(self):
        source = SOURCE.read_text()
        begin = source.index("static int h2_notify(")
        end = source.index("static int h2_disconnect(", begin)
        stub = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
enum { H2_PAL_ERR_INVALID_ARG=-1, H2_JIELI_GATT_TX_VALUE_HANDLE=6,
 H2_PAL_BLE_ATT_HEADER_LEN=3, ATT_OP_AUTO_READ_CCC=1 };
static struct { uint16_t conn_handle, mtu; } h2_ble;
static int sends, vendor_result;
static uint16_t sent_length;
static int h2_ble_cmd_result(int result) { return result; }
static int ble_op_att_send_data(uint16_t attr, const uint8_t *data,
                               uint16_t len, int mode) {
 assert(attr == 6 && mode == 1); assert(len == 0 || data != NULL);
 ++sends; sent_length=len; return vendor_result;
}
'''
        main = r'''
int main(void) {
 uint8_t data[200]={0};
 h2_ble.conn_handle=42; h2_ble.mtu=23;
 assert(h2_notify(NULL,42,6,data,20)==0 && sends==1 && sent_length==20);
 vendor_result=-100;
 assert(h2_notify(NULL,42,6,data,1)==-100 && sends==2);
 vendor_result=0;
 assert(h2_notify(NULL,42,6,NULL,0)==0 && sends==3);
 const size_t invalid[]={21,65536,SIZE_MAX,SIZE_MAX-1,SIZE_MAX-2};
 for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
  assert(h2_notify(NULL,42,6,data,invalid[i])==-1);
  assert(sends==3);
 }
 assert(h2_notify(NULL,41,6,data,1)==-1);
 assert(h2_notify(NULL,42,9,data,1)==-1);
 assert(h2_notify(NULL,42,6,NULL,1)==-1);
 for (unsigned mtu=0;mtu<3;++mtu) {
  h2_ble.mtu=mtu;
  assert(h2_notify(NULL,42,6,NULL,0)==-1);
  assert(h2_notify(NULL,42,6,data,SIZE_MAX-2)==-1);
 }
 h2_ble.mtu=23; h2_ble.conn_handle=0;
 assert(h2_notify(NULL,0,6,data,1)==-1 && sends==3);
 h2_ble.conn_handle=42; h2_ble.mtu=200;
 assert(h2_notify(NULL,42,6,data,197)==0 && sent_length==197 && sends==4);
 assert(h2_notify(NULL,42,6,data,198)==-1 && sends==4);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-ble-notify-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + source[begin:end] + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], timeout=10,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_att_failure_does_not_publish_connected(self):
        source = SOURCE.read_text()
        begin = source.index("        uint16_t handle = subevent ==")
        end = source.index("      } else if (subevent == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE)", begin)
        branch = source[begin:end]
        stub = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { H2_PAL_OK=0, H2_PAL_BLE_ROLE_PERIPHERAL=1,
 H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED=2, H2_JIELI_ATT_MTU=200,
 HCI_SUBEVENT_LE_CONNECTION_COMPLETE=1 };
typedef struct { uint16_t conn_handle, role, mtu; } h2_pal_ble_connection_t;
static struct { struct { int started; } adv; uint16_t conn_handle, mtu; } h2_ble;
static uint8_t h2_att_buffer[1024];
static int init_result, disconnect_result, init_calls, disconnect_calls, posts;
static uint16_t disconnected_handle;
static int h2_ble_cmd_result(int rc) { return rc == 0 ? 0 : -1; }
static uint16_t hci_subevent_le_connection_complete_get_connection_handle(uint8_t *p) {
 (void)p; return 42;
}
static uint16_t hci_subevent_le_enhanced_connection_complete_get_connection_handle(uint8_t *p) {
 (void)p; return 43;
}
static int ble_op_att_send_init(uint16_t h, void *p, size_t n, uint16_t mtu) {
 assert(h == 42 || h == 43); assert(p == h2_att_buffer);
 assert(n == sizeof(h2_att_buffer) && mtu == 200); ++init_calls;
 return init_result;
}
static int ble_op_disconnect(uint16_t h) {
 disconnected_handle=h; ++disconnect_calls; return disconnect_result;
}
static void h2_ble_post(int type, const void *p, size_t n) {
 const h2_pal_ble_connection_t *c=p;
 assert(type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED);
 assert(n == sizeof(*c) && c->conn_handle == h2_ble.conn_handle && c->mtu == 23);
 ++posts;
}
static void connect_event(uint8_t subevent) {
 uint8_t packet[32]={0};
 unsigned interval=24, latency=0, supervision_timeout=500;
 do {
'''
        main = r'''
 } while (0);
}
int main(void) {
 const int results[]={0,-100,-99,-98,-97,-96,-1};
 for (unsigned type=1; type<=2; ++type) {
  for (unsigned i=0; i<sizeof(results)/sizeof(results[0]); ++i) {
   for (unsigned fail_disconnect=0; fail_disconnect<2; ++fail_disconnect) {
    memset(&h2_ble,0,sizeof(h2_ble)); h2_ble.adv.started=1;
    init_calls=disconnect_calls=posts=0;
    init_result=results[i]; disconnect_result=fail_disconnect ? -100 : 0;
    connect_event(type);
    assert(init_calls == 1 && h2_ble.adv.started == 0);
    assert(h2_ble.conn_handle == (type == 1 ? 42 : 43));
    if (init_result == 0) {
     assert(posts == 1 && disconnect_calls == 0 && h2_ble.mtu == 23);
    } else {
     assert(posts == 0 && disconnect_calls == 1 && h2_ble.mtu == 0);
     assert(disconnected_handle == h2_ble.conn_handle);
    }
   }
  }
 }
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-ble-connection-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + branch + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], timeout=10,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
