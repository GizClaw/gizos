"""An idle App KCP read yields EOF-as-timeout, not console closure."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
APP = 'libs/h2loader/device/src/h2_device_serial.c'
CLIENT = 'libs/h2loader/device/src/h2_loader_app_client.c'


def function(text, signature):
    start = text.index(signature); end = text.index('{', start) + 1; depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[start:end]


class IdleConsoleTest(unittest.TestCase):
    def test_idle_then_open(self):
        app = (ROOT / APP).read_text(); client = (ROOT / CLIENT).read_text()
        engine = (ROOT / 'libs/h2loader/device/src/h2_device_serial.c').read_text()
        code = function(engine, 'static h2_pal_result_t poll_physical(').replace('h2_device_serial_t', 'h2_jieli_app_transport_t')
        code += function(app, 'static int app_read_byte(')
        code += function(client, 'static h2_pal_result_t console_read(')
        code += function(client, 'static int run_return_console(')
        fixture = (ROOT / 'libs/h2loader/device/tests/fixtures/jieli_idle_console.c').read_text()
        fixture = fixture.replace('/* FUNCTIONS */', '#define transport_stopped(self) 0\n#define h2_device_serial_now(self) timer_get_ms()\n#define h2_device_serial_poll_session poll_physical\n#define transport_read(self,buffer,len,count,timeout) ((self)->physical_io.read((self)->physical_io.user,buffer,len,count,timeout))\n/* FUNCTIONS */')
        fixture = fixture.replace('  void* stream;\n', '  struct { unsigned idle_timeout_ms; } config;\n  unsigned last_frame_ms;\n  void* stream;\n')
        code = code.replace('h2_device_app_serial_t', 'h2_jieli_app_console_t')
        fixture = fixture.replace('deactivate_current', 'h2_device_serial_deactivate').replace('activate_pending', 'h2_device_serial_activate_pending')
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'; binary = Path(directory) / 'test'
            unit.write_text(fixture.replace('/* FUNCTIONS */', code))
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(unit), '-I', str(ROOT / 'libs/sync/atomic/include'), str(ROOT / 'libs/sync/atomic/providers/c11/src/h2_atomic_c11.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
            # Negative control: reproduce the reviewer's assumed EOF mapping.
            unit.write_text(fixture.replace('/* FUNCTIONS */', code.replace(
                'if (value == EOF) return H2_PAL_ERR_TIMEOUT;',
                'if (value == EOF) return H2_PAL_ERR_CLOSED;')))
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(unit), '-I', str(ROOT / 'libs/sync/atomic/include'), str(ROOT / 'libs/sync/atomic/providers/c11/src/h2_atomic_c11.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0, 'The fixture must detect premature idle closure')


if __name__ == '__main__':
    unittest.main()
