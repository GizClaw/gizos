"""Execute the real GATT binding and ATT callback with pthread interleavings."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class GattLifecycleTest(unittest.TestCase):
    def test_read_bounds(self):
        source = (ROOT / 'boards/jieli/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c').read_text()
        code = source[source.index('static uint16_t h2_att_read('):source.index('static int h2_att_write(')]
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint16_t hci_con_handle_t;
#define H2_JIELI_GATT_DEVICE_NAME_HANDLE 3u
#define H2_JIELI_GATT_TX_CCCD_HANDLE 7u
#define h2_att_trace_record(...) ((void)0)
#include "h2_jieli_wl82_gatt.h"
static char h2_gap_name[30]="H2Loader";
static h2_jieli_gatt_database_t h2_gatt_database={.count=1,.cccd_handles={7}};
static struct { int gatt_registered; h2_pal_ble_gatt_characteristic_t characteristics[16]; } h2_ble={.gatt_registered=1};
typedef struct h2_gatt_call { struct h2_gatt_call *next; const void *task; } h2_gatt_call_t;
static h2_gatt_call_t *h2_gatt_calls;
static void h2_gatt_lock(void) {}
static void h2_gatt_unlock(void) {}
static const void *h2_jieli_sdk_task_current(void) { return 0; }
static void h2_gatt_release(h2_gatt_call_t *call) { (void)call; h2_gatt_calls=0; }

static uint16_t att_get_ccc_config(uint16_t handle) {
    assert(handle == H2_JIELI_GATT_TX_CCCD_HANDLE);
    return 1u;
}
/* REAL_READ */
int main(void) {
    const uint16_t handle = H2_JIELI_GATT_TX_CCCD_HANDLE;
    for (uint16_t offset = 0; offset <= 3; ++offset) {
        for (uint16_t size = 0; size <= 3; ++size) {
            uint8_t bytes[5];
            memset(bytes, 0xa5, sizeof(bytes));
            const uint16_t remaining = offset < 2 ? 2 - offset : 0;
            const uint16_t expected = remaining < size ? remaining : size;
            assert(h2_att_read(1, handle, offset, bytes + 1, size) == expected);
            assert(bytes[0] == 0xa5);
            for (uint16_t i = 0; i < expected; ++i)
                assert(bytes[1 + i] == (offset + i == 0 ? 1 : 0));
            for (uint16_t i = expected + 1; i < sizeof(bytes); ++i)
                assert(bytes[i] == 0xa5);
        }
        assert(h2_att_read(1, handle, offset, NULL, 0) == (offset < 2 ? 2 : 0));
    }
    uint8_t byte = 0xa5;
    assert(h2_att_read(1, H2_JIELI_GATT_DEVICE_NAME_HANDLE, 1, &byte, 1) == 1);
    assert(byte == '2');
    assert(h2_att_read(1, 99, 0, &byte, 1) == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'read.c'
            test.write_text(fixture.replace('/* REAL_READ */', code))
            binary = Path(directory) / 'read'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(test), '-I', str(ROOT / 'libs/pal/include'), '-I', str(ROOT / 'native_component_src/jieli/wl82/h2_pal_image/include'), '-I', str(ROOT / 'libs/atomic/include'), str(ROOT / 'libs/atomic/providers/c11/src/h2_atomic_c11.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_generic_schema_preserves_h2_handles_and_accepts_other_services(self):
        program = r'''
#include <assert.h>
#include "h2_jieli_wl82_gatt.h"
static h2_pal_result_t write_value(void *u,const h2_pal_ble_gatt_access_t *a,const uint8_t *p,size_t n) {
 (void)u;(void)a;(void)p;(void)n;return 0;
}
int main(void) {
 uint8_t service_uuid[16],tx_uuid[16],rx_uuid[16];
 memset(service_uuid,0x11,16);memset(tx_uuid,0x22,16);memset(rx_uuid,0x33,16);
 h2_pal_ble_gatt_characteristic_t chars[2]={
  {.uuid={tx_uuid,16},.properties=H2_PAL_BLE_GATT_PROPERTY_NOTIFY,.max_value_len=512},
  {.uuid={rx_uuid,16},.properties=H2_PAL_BLE_GATT_PROPERTY_WRITE|H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP,.permissions=H2_PAL_BLE_GATT_PERMISSION_WRITE,.max_value_len=512,.write=write_value}
 };
 h2_pal_ble_gatt_service_t service={.uuid={service_uuid,16},.primary=1,.characteristics=chars,.characteristic_count=2};
 h2_jieli_gatt_database_t db;
 assert(h2_jieli_gatt_build(&db,&service,1)==0 && db.size==165 && db.count==2);
 assert(db.service_handles[0]==4 && db.value_handles[0]==6 && db.cccd_handles[0]==7 && db.value_handles[1]==9 && db.cccd_handles[1]==0);
 assert(memcmp(db.bytes+39,service_uuid,16)==0 && memcmp(db.bytes+66,tx_uuid,16)==0 && memcmp(db.bytes+125,rx_uuid,16)==0);
 assert(db.bytes[84]==0x10 && db.bytes[85]==2 && db.bytes[106]==0x0a && db.bytes[107]==1 && db.bytes[143]==0x0c && db.bytes[144]==3);
 /* Arbitrary 16-bit services, multiple services, static values, and mixed
  * properties must build without any H2 UUID or two-characteristic gate. */
 const uint8_t uuid16[2]={0x0d,0x18},value[3]={1,2,3};
 h2_pal_ble_gatt_characteristic_t generic[3]={
  {.uuid={uuid16,2},.properties=H2_PAL_BLE_GATT_PROPERTY_READ,.permissions=H2_PAL_BLE_GATT_PERMISSION_READ,.initial_value=value,.initial_value_len=3,.max_value_len=3},
  {.uuid={uuid16,2},.properties=H2_PAL_BLE_GATT_PROPERTY_INDICATE,.max_value_len=512},
  {.uuid={uuid16,2},.properties=H2_PAL_BLE_GATT_PROPERTY_WRITE,.max_value_len=512,.write=write_value}
 };
 h2_pal_ble_gatt_service_t services[2]={
  {.uuid={uuid16,2},.primary=1,.characteristics=generic,.characteristic_count=3},
  {.uuid={uuid16,2},.primary=0,.characteristics=generic,.characteristic_count=1}
 };
 assert(h2_jieli_gatt_build(&db,services,2)==0 && db.count==4);
 assert(db.service_handles[0]==4 && db.service_handles[1]==12 && db.value_handles[3]==14);
 assert(db.value_handles[0]==6 && db.cccd_handles[0]==0 && db.value_handles[1]==8 && db.cccd_handles[1]==9 && db.value_handles[2]==11);
 generic[0].permissions|=H2_PAL_BLE_GATT_PERMISSION_READ_ENCRYPTED;
 assert(h2_jieli_gatt_build(&db,services,2)==H2_PAL_ERR_UNSUPPORTED);
 generic[0].permissions=H2_PAL_BLE_GATT_PERMISSION_READ;
 generic[0].uuid.len=3;assert(h2_jieli_gatt_build(&db,services,2)==H2_PAL_ERR_INVALID_ARG);generic[0].uuid.len=2;
 services[0].characteristic_count=17;assert(h2_jieli_gatt_build(&db,services,2)==H2_PAL_ERR_NO_SPACE);
 assert(h2_jieli_gatt_build(&db,services,5)==H2_PAL_ERR_NO_SPACE);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / "gatt.c"
            binary = Path(directory) / "gatt"
            unit.write_text(program)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "libs/pal/include"),
                            "-I", str(ROOT / "native_component_src/jieli/wl82/h2_pal_image/include"),
                            str(unit), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_lifetime(self):
        source = (ROOT / 'boards/jieli/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c').read_text()
        helpers = ''
        if '/* GATT lifetime helpers. */' in source:
            helpers = source.split('/* GATT lifetime helpers. */', 1)[1].split('/* End GATT lifetime helpers. */', 1)[0]
        code = helpers + source[source.index('static int h2_register_gatt('):source.index('static int h2_notify(')]
        code += source[source.index('static int h2_att_write('):source.index('static void h2_packet_handler(')]
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_ble_gatt_lifecycle.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(fixture.replace('/* REAL_PROVIDER */', code))
            binary = Path(directory) / 'test'
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-pthread',
                *os.environ.get('JIELI_TEST_CFLAGS', '').split(),
                '-I', str(ROOT / 'libs/pal/include'), '-I', str(ROOT / 'native_component_src/jieli/wl82/h2_pal_image/include'), '-I', str(ROOT / 'native_component_src/jieli/wl82/h2_pal_core/include'),
                str(test), '-I', str(ROOT / 'libs/pal/include'), '-I', str(ROOT / 'native_component_src/jieli/wl82/h2_pal_image/include'), '-I', str(ROOT / 'libs/atomic/include'), str(ROOT / 'libs/atomic/providers/c11/src/h2_atomic_c11.c'), '-o', str(binary)], check=True)
            for case in ['unregister', 'self_unregister', 'live_rebind']:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
