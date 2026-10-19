"""Real netif provider snapshots run on the TCP/IP owner and retain radio use."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class NetifSnapshotTest(unittest.TestCase):
    def test_snapshot(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_netif.c').read_text()
        source = source[source.index('static '):source.index('\n#else')]
        wifi = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_wifi.c').read_text()
        pack = wifi[wifi.index('static uint32_t pack_ip4('):wifi.index('static h2_pal_wifi_security_t')]
        refresh = wifi[wifi.index('static void update_sta_snapshot(h2_pal_wifi_sta_status_t *status) {'):wifi.index('static int sta_get_status(')]
        source += pack + refresh
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_netif_snapshot.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            unit.write_text(fixture.replace('/* REAL_PROVIDER */', source))
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-pthread',
                *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                '-I', str(ROOT / 'libs/pal/include'), str(unit), '-I', str(ROOT / 'libs/atomic/include'), str(ROOT / 'libs/atomic/providers/c11/src/h2_atomic_c11.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertNotIn('WARNING: ThreadSanitizer', result.stderr)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
