"""Execute the real GATT binding and ATT callback with pthread interleavings."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class GattLifecycleTest(unittest.TestCase):
    def test_read_bounds(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c').read_text()
        code = source[source.index('static uint16_t h2_att_read('):source.index('static int h2_att_write(')]
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint16_t hci_con_handle_t;
#define H2_JIELI_GATT_DEVICE_NAME_HANDLE 3u
#define H2_JIELI_GATT_TX_CCCD_HANDLE 7u
#define h2_att_trace_record(...) ((void)0)
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
                            str(test), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_lifetime(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c').read_text()
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
                '-I', str(ROOT / 'libs/pal/include'), '-I', str(ROOT / 'native_component_src/jieli/wl82/h2_pal_core/include'),
                str(test), '-o', str(binary)], check=True)
            for case in ['unregister', 'self_unregister']:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
