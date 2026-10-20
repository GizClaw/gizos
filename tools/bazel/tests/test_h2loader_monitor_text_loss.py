"""Exercise lossless frame demux through the real serial monitor log callback."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class MonitorTextLoss(unittest.TestCase):
    def test_monitor_text_loss(self):
        serial = (ROOT / 'libs/h2loader_host/src/h2_h2loader_host_serial.c').read_text()
        start = serial.index('/* A USB-UART adapter survives')
        end = serial.index('static h2_pal_result_t serial_wait_ready_marker(', start)
        # The READY marker constant is local to serial_stream_log.
        callback = serial[start:end]
        start = serial.index('typedef struct serial_status_output serial_status_output_t;')
        end = serial.index('static h2_pal_result_t serial_finish_command_response(', start)
        definitions = serial[start:end]
        frame = (ROOT / 'libs/iostreamikcp/src/h2_iostreamikcp_frame.c').read_text()
        frame = frame.replace(
            '#include "h2_iostreamikcp_internal.h"',
            '#include "h2_iostreamikcp.h"\n#define H2_IOSTREAMIKCP_FRAME_MAGIC_LEN 6u\n#define H2_IOSTREAMIKCP_FRAME_LEN_OFFSET 12u')
        fixture = (ROOT / 'tools/bazel/tests/fixtures/h2loader_monitor_text_loss.c').read_text()
        # Use the real connection and callback dependencies, not a copied layout.
        start = fixture.index('struct h2_h2loader_host_serial_connection {')
        end = fixture.index('/* SERIAL_LOG */', start)
        unexpected_read = '''
h2_pal_result_t h2_iostreamikcp_read(
    h2_iostreamikcp_t *stream, uint8_t *out, size_t size, size_t *len) {
    (void)stream; (void)out; (void)size; (void)len;
    assert(0 && "raw demux fixture must not read decoded data");
    return H2_PAL_ERR_INVALID_STATE;
}
'''
        fixture = fixture[:start] + unexpected_read + definitions + fixture[end:]
        fixture = fixture.replace('/* FRAME */', frame).replace('/* SERIAL_LOG */', callback)
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
            for case in ['chunked', 'bytewise']:
                with self.subTest(case=case):
                    result = subprocess.run(
                        [str(binary), case], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
