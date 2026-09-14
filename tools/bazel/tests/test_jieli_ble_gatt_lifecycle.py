"""Execute the real GATT binding and ATT callback with pthread interleavings."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class GattLifecycleTest(unittest.TestCase):
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
