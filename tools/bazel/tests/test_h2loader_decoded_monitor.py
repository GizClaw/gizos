"""Exercise decoded console delivery through the real serial monitor."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class DecodedMonitor(unittest.TestCase):
    def test_decoded_monitor(self):
        serial = (ROOT / 'libs/h2loader_host/src/h2_h2loader_host_serial.c').read_text()
        start = serial.index('/* A USB-UART adapter survives')
        end = serial.index('static h2_pal_result_t serial_wait_ready_marker(', start)
        callback = serial[start:end]
        start = serial.index('/* Console output during a monitor session')
        end = serial.index('h2_pal_result_t h2_h2loader_host_serial_disconnect(', start)
        monitor = serial[start:end]
        start = serial.index('static h2_pal_result_t serial_now(')
        end = serial.index('static uint32_t serial_stream_now_ms(', start)
        now = serial[start:end]
        start = serial.index('static h2_pal_result_t serial_pump(')
        end = serial.index('static h2_pal_result_t serial_wait_delivery(', start)
        pump = serial[start:end]
        fixture = (ROOT / 'tools/bazel/tests/fixtures/h2loader_decoded_monitor.c').read_text()
        fixture = fixture.replace('/* SERIAL_LOG */', callback).replace('/* MONITOR */', monitor)
        fixture = fixture.replace('/* SERIAL_NOW */', now).replace('/* SERIAL_PUMP */', pump)
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            unit.write_text(fixture)
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'libs/pal/include'),
                '-I' + str(ROOT / 'libs/h2loader_host/include'),
                '-I' + str(ROOT / 'libs/iostreamikcp/include'),
                str(unit), '-o', str(binary),
            ], check=True)
            for case in ['tunnelled', 'mixed', 'raw_only']:
                with self.subTest(case=case):
                    result = subprocess.run(
                        [str(binary), case], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
