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
        # Stop before the monitor-only decoded-log drain: this fixture exercises
        # the handshake handoff and never calls it, so pulling it in would trip
        # -Wunused-function.
        connect = section(serial, 'static h2_pal_result_t serial_connect(', '/* Console output during a monitor session')
        deadline = section(serial, 'static h2_pal_result_t serial_deadline_budget(\n    h2_h2loader_host_serial_connection_t *connection,\n', 'static h2_pal_result_t serial_read_status(')
        frame = (ROOT/'libs/iostreamikcp/src/h2_iostreamikcp_frame.c').read_text().replace('#include "h2_iostreamikcp_internal.h"', '#include "h2_iostreamikcp.h"\n#define H2_IOSTREAMIKCP_FRAME_MAGIC_LEN 6u\n#define H2_IOSTREAMIKCP_FRAME_LEN_OFFSET 12u')
        app = (ROOT/'projects/h2loader/apps/cli/app/src/h2_h2loader_cli_app.c').read_text()
        # Main keeps the serial log callback in app.c; no BLE sink dependency.
        output = section(app, 'static h2_pal_result_t command_output(', 'static h2_pal_result_t file_read(')
        verify = section(app, 'static int metadata_equal(', 'static int reboot_command_kind(')
        verify += section(app, 'h2_pal_result_t h2_h2loader_cli_verify_reboot_status(', 'static int active_matches_metadata(')
        verify += section(app, 'static h2_pal_result_t reconnect_and_verify_reboot(', 'static h2_pal_result_t monitor_transport(')
        fixture = (ROOT/'tools/bazel/tests/fixtures/h2loader_serial_handoff.c').read_text()
        context = 'const struct runtime *runtime; } h2_h2loader_cli_context_t;'
        self.assertEqual(fixture.count(context), 1)
        fixture = fixture.replace(context, 'const struct runtime *runtime; h2_pal_result_t log_result; } h2_h2loader_cli_context_t;')
        transport = 'typedef struct { int unused; } h2_h2loader_cli_transport_t;'
        self.assertEqual(fixture.count(transport), 1)
        fixture = fixture.replace(transport, '''
typedef struct {
    h2_h2loader_host_transport_t transport;
    uint32_t wait_timeout_ms;
} fixture_options_t;
static const fixture_options_t legacy_options={.transport=H2_H2LOADER_HOST_TRANSPORT_SERIAL};
typedef struct {
    const fixture_options_t *options;
    h2_h2loader_host_transport_log_fn on_log;
    uint64_t status_deadline_ms;
} h2_h2loader_cli_transport_t;
/* This harness retains the real non-monitor reconnect function. The new
 * monitor helper is exercised through the real KCP CLI control-lines test. */
static h2_pal_result_t wait_serial_reboot(
    h2_h2loader_cli_context_t *c,h2_h2loader_cli_transport_t *t,
    h2_h2loader_host_command_t k,const h2_h2loader_host_status_t *b,
    h2_h2loader_host_status_t *o,uint64_t s,uint64_t d) {
    (void)c;(void)t;(void)k;(void)b;(void)o;(void)s;(void)d;
    assert(!"unexpected monitor path in legacy reconnect fixture");
    return H2_PAL_ERR_INVALID_STATE;
}
''')
        fixture = fixture.replace('h2_h2loader_cli_transport_t transport={0};',
            'h2_h2loader_cli_transport_t transport={.options=&legacy_options};')
        for key, value in [('FRAME',frame),('SERIAL',prefix+deadline+connect),('OUTPUT',output),('VERIFY',verify)]:
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
