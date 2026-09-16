"""The App transport defers SESSION_OPEN until the confirmation console has drained."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = 'projects/h2loader/native_component_src/jieli/wl82/h2loader_app/src/jieli_app_iostreamikcp.c'
CASES = ['gated', 'admit', 'fallback', 'reack', 'invalid']


def function(source, name):
    for prefix in ('static h2_pal_result_t ', 'static int ', 'static void '):
        if prefix + name + '(' in source:
            break
    else:
        raise AssertionError(name)
    begin = source.index(prefix + name + '(')
    brace = source.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


class SessionGateTest(unittest.TestCase):
    def test_gate(self):
        source = (ROOT / SOURCE).read_text()
        begin = source.index('typedef struct h2_jieli_app_transport {')
        structure = source[begin:source.index('} h2_jieli_app_transport_t;', begin) + len('} h2_jieli_app_transport_t;')]
        self.assertIn('sessions_admitted', structure)
        fallback = re.search(r'H2_SESSION_GATE_FALLBACK_MS = (\d+),', source)
        self.assertIsNotNone(fallback, 'fallback deadline must be a plain enum literal')
        functions = '\n'.join(function(source, name) for name in
                              ['write_le32', 'transport_write', 'send_control', 'on_frame'])
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_session_gate.c').read_text()
        fixture = fixture.replace('/* STRUCTURE */', structure).replace('TRANSPORT', 'h2_jieli_app_transport_t')
        fixture = fixture.replace('/* FUNCTIONS */', functions).replace('/* FALLBACK_MS */', fallback.group(1) + 'u')
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            unit.write_text(fixture)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                            '-I', str(ROOT / 'libs/pal/include'),
                            '-I', str(ROOT / 'libs/iostreamikcp/include'),
                            '-I', str(ROOT / 'native_component_src/jieli/wl82/h2_pal_core/include'),
                            str(unit), '-o', str(binary)], check=True)
            for case in CASES:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
