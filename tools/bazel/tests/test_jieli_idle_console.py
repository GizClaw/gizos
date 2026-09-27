"""An idle App KCP read yields EOF-as-timeout, not console closure."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
APP = 'projects/h2loader/native_component_src/jieli/wl82/h2loader_app/src/jieli_app_iostreamikcp.c'
CLIENT = 'projects/h2loader/libs/h2loader/src/h2_loader_app_client.c'


def function(text, signature):
    start = text.index(signature); end = text.index('{', start) + 1; depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[start:end]


class IdleConsoleTest(unittest.TestCase):
    def test_idle_then_open(self):
        app = (ROOT / APP).read_text(); client = (ROOT / CLIENT).read_text()
        code = function(app, 'static int poll_physical(')
        code += function(app, 'static int app_read_byte(')
        code += function(client, 'static h2_pal_result_t console_read(')
        code += function(client, 'static int run_return_console(')
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_idle_console.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'; binary = Path(directory) / 'test'
            unit.write_text(fixture.replace('/* FUNCTIONS */', code))
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(unit), '-I', str(ROOT / 'libs/atomic/include'), str(ROOT / 'libs/atomic/providers/c11/src/h2_atomic_c11.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
            # Negative control: reproduce the reviewer's assumed EOF mapping.
            unit.write_text(fixture.replace('/* FUNCTIONS */', code.replace(
                'if (value == EOF) return H2_PAL_ERR_TIMEOUT;',
                'if (value == EOF) return H2_PAL_ERR_CLOSED;')))
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(unit), '-I', str(ROOT / 'libs/atomic/include'), str(ROOT / 'libs/atomic/providers/c11/src/h2_atomic_c11.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0, 'The fixture must detect premature idle closure')


if __name__ == '__main__':
    unittest.main()
