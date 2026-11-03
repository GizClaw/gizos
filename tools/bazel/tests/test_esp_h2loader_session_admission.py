"""Exercise real ESP session admission under allocation and ACK failures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = 'projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_runtime/src/h2_esp_h2loader_iostreamikcp.c'


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class SessionAdmission(unittest.TestCase):
    def test_failure_and_reconnect(self):
        source = (ROOT / SOURCE).read_text()
        code = '\n'.join(function(source, signature) for signature in [
            'void h2_esp_h2loader_command_transport_deactivate(',
            'h2_pal_result_t h2_esp_h2loader_command_transport_activate_pending(',
            'int h2_esp_h2loader_command_transport_has_session(',
            'int h2_esp_h2loader_command_transport_replacement_pending(',
            'static int app_read_byte(',
        ])
        fixture = (ROOT / 'tools/bazel/tests/fixtures/esp_h2loader_session_admission.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            budgets = source[source.index('#ifndef H2_LOADER_SERIAL_RX_BUFFER'):
                             source.index('#define H2_LOADER_CONSOLE_UART')]
            unit.write_text(fixture.replace('/* BUDGETS */', budgets)
                            .replace('/* FUNCTIONS */', code))
            for tick_ms in [1, 100]:
                for compact in [False, True]:
                    defines = ['-DTICK_MS=' + str(tick_ms)]
                    if compact:
                        defines += ['-DH2_LOADER_SERIAL_RX_BUFFER=1024',
                                    '-DH2_LOADER_STREAM_RX_BUFFER=1024',
                                    '-DH2_LOADER_TRANSPORT_RECEIVE_WINDOW=8']
                    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        *defines, '-I' + str(ROOT / 'libs/pal/include'),
                        '-I' + str(ROOT / 'libs/iostreamikcp/include'),
                        str(unit), '-o', str(binary)], check=True)
                    for case in ['oom', 'ack', 'flood', 'success', 'invalid']:
                        with self.subTest(tick_ms=tick_ms, compact=compact, case=case):
                            result = subprocess.run([str(binary), case], capture_output=True,
                                                    text=True, timeout=10)
                            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
