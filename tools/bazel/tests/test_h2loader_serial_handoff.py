"""Exercise the real serial handshake, frame demux and CLI reconnect owners."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]
def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]
class SerialHandoff(unittest.TestCase):
    def test_handoff(self):
        serial = (ROOT/'libs/h2loader_host/src/h2_h2loader_host_serial.c').read_text()
        prefix = serial[:serial.index('static h2_pal_result_t serial_pump(')]
        prefix = prefix.replace('#include "h2_h2loader_host_internal.h"', '')
        prefix = re.sub(r'\bstatic\s+h2_pal_result_t\s+serial_finish_command_response\s*\(\s*void\s*\*\s*transport\s*\)\s*;', '', prefix)
        # Stop before the monitor-only decoded-log drain: this fixture exercises
        # the handshake handoff and never calls it, so pulling it in would trip
        # -Wunused-function.
        connect = section(serial, 'h2_pal_result_t h2_h2loader_host_serial_connect(', '/* Console output during a monitor session')
        frame = (ROOT/'libs/iostreamikcp/src/h2_iostreamikcp_frame.c').read_text().replace('#include "h2_iostreamikcp_internal.h"', '#include "h2_iostreamikcp.h"\n#define H2_IOSTREAMIKCP_FRAME_MAGIC_LEN 6u\n#define H2_IOSTREAMIKCP_FRAME_LEN_OFFSET 12u')
        app = (ROOT/'projects/h2loader/apps/cli/app/src/h2_h2loader_cli_app.c').read_text()
        # Main keeps the serial log callback in app.c; no BLE sink dependency.
        output = section(app, 'static h2_pal_result_t transport_log(', 'static h2_pal_result_t file_read(')
        verify = (ROOT/'libs/h2loader_host/src/h2_h2loader_host_lifecycle.c').read_text().replace('#include "h2_h2loader_host_lifecycle.h"', '')
        verify += section(app, 'typedef struct confirmation_wait', 'static int monitor_transport(') if 'static int monitor_transport(' in app else section(app, 'typedef struct confirmation_wait', 'static h2_pal_result_t monitor_transport(')
        fixture = (ROOT/'tools/bazel/tests/fixtures/h2loader_serial_handoff.c').read_text()
        # Fake input parses frames without decoding KCP payloads, so its queue is empty.
        empty_read = '''
h2_pal_result_t h2_iostreamikcp_read(
    h2_iostreamikcp_t *stream, uint8_t *out, size_t size, size_t *len) {
    assert(stream != NULL && stream->config.conv != 0u);
    assert(out != NULL && size > 0u && len != NULL);
    *len = 0u;
    return H2_PAL_ERR_WOULD_BLOCK;
}
'''
        for key, value in [('FRAME',frame),('SERIAL',empty_read+prefix+connect),('OUTPUT',output),('VERIFY',verify)]:
            fixture=fixture.replace('/* '+key+' */',value)
        with tempfile.TemporaryDirectory() as directory:
            unit=Path(directory)/'test.c';binary=Path(directory)/'test'
            unit.write_text(fixture)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'libs/pal/include'),'-I'+str(ROOT/'libs/h2loader_host/include'),'-I'+str(ROOT/'libs/iostreamikcp/include'),str(unit),'-o',str(binary)],check=True)
            for case in ['frame_text','split_frame_text','reconnect','upgrade_failure','reset_ready','ready_split']:
                with self.subTest(case=case):
                    result=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=10)
                    self.assertEqual(result.returncode,0,result.stderr)
if __name__=='__main__': unittest.main()
