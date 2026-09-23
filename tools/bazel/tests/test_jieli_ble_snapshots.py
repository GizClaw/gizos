"""Exercise real connection/MTU events and readers with threaded SDK events."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class BleSnapshotsTest(unittest.TestCase):
    def test_snapshots(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c').read_text()
        trace = source[source.index('typedef struct h2_jieli_att_trace'):source.index('static int h2_unregister_gatt(void *user);')]
        getter = source[source.index('static int h2_exchange_mtu('):source.index('/* The central owns PHY selection.')]
        begin = source.index('        /* Legacy connectable advertising stops automatically on connection. */')
        end = source.index('        h2_ble_log(\n            "H2_JIELI_BLE_LINK_PARAMS', begin)
        connected = 'static void connected(uint16_t handle) {\n' + source[begin:end] + '\n}\n'
        begin = source.index('    case HCI_EVENT_DISCONNECTION_COMPLETE: {')
        end = source.index('    case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS:', begin)
        events = 'static void event(int kind, uint8_t *packet, uint16_t size) {\n(void)size;\nswitch(kind) {\n' + source[begin:end] + '\ndefault: break;\n}\n}\n'
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_ble_snapshots.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(fixture.replace('/* REAL_PROVIDER */', trace + getter + connected + events))
            binary = Path(directory) / 'test'
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-pthread',
                *os.environ.get('JIELI_TEST_CFLAGS', '').split(), '-I', str(ROOT / 'libs/pal/include'),
                str(test), '-I', str(ROOT / 'libs/atomic/include'), str(ROOT / 'libs/atomic/providers/c11/src/h2_atomic_c11.c'), '-o', str(binary)], check=True)
            for case in ['failed_disconnect', 'short_events', 'stale_mtu', 'invalid_mtu', 'stale_disconnect', 'current_events', 'snapshots', 'trace']:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
