"""Execute the board's connection-complete path with ATT failures injected."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c"


class BleConnectionTest(unittest.TestCase):
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
