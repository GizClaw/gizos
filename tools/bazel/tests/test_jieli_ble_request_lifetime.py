"""Execute the real connection request with a delayed SDK command consumer."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class RequestLifetimeTest(unittest.TestCase):
    def test_request(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c').read_text()
        begin = source.index('/* Connection request lifetime. */') if '/* Connection request lifetime. */' in source else source.index('static int h2_update_connection(')
        code = source[begin:source.index('static int h2_exchange_mtu(', begin)]
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_ble_request_lifetime.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(fixture.replace('/* REAL_PROVIDER */', code))
            binary = Path(directory) / 'test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                *os.environ.get('JIELI_TEST_CFLAGS', '').split(), '-I', str(ROOT / 'libs/pal/include'),
                str(test), '-o', str(binary)], check=True)
            for case in ['queued', 'submitting', 'registration_error', 'request_error', 'consumer_busy']:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
