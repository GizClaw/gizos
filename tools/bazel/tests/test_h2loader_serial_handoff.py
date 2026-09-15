"""Exercise the real serial handshake, frame demux and CLI reconnect owners."""
from pathlib import Path
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
        prefix = prefix.replace('static h2_pal_result_t serial_finish_command_response(void *transport);', '')
        connect = section(serial, 'h2_pal_result_t h2_h2loader_host_serial_connect(', 'h2_pal_result_t h2_h2loader_host_serial_monitor_logs(')
        frame = (ROOT/'libs/iostreamikcp/src/h2_iostreamikcp_frame.c').read_text().replace('#include "h2_iostreamikcp_internal.h"', '#include "h2_iostreamikcp.h"\n#define H2_IOSTREAMIKCP_FRAME_MAGIC_LEN 6u\n#define H2_IOSTREAMIKCP_FRAME_LEN_OFFSET 12u')
        app = (ROOT/'projects/h2loader/apps/cli/app/src/h2_h2loader_cli_app.c').read_text()
        # Main keeps the serial log callback in app.c; no BLE sink dependency.
        output = section(app, 'static h2_pal_result_t command_output(', 'static h2_pal_result_t file_read(')
        verify = section(app, 'static int metadata_equal(', 'static int reboot_command_kind(')
        verify += section(app, 'h2_pal_result_t h2_h2loader_cli_verify_reboot_status(', 'static int monitor_transport(') if 'static int monitor_transport(' in app else section(app, 'h2_pal_result_t h2_h2loader_cli_verify_reboot_status(', 'static h2_pal_result_t monitor_transport(')
        fixture = (ROOT/'tools/bazel/tests/fixtures/h2loader_serial_handoff.c').read_text()
        for key, value in [('FRAME',frame),('SERIAL',prefix+connect),('OUTPUT',output),('VERIFY',verify)]:
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
