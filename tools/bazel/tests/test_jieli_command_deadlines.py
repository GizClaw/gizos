"""Command budgets reach real Loader/App KCP physical I/O wiring."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCES = [
    'projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit/src/loader_launcher.c',
    'projects/h2loader/native_component_src/jieli/wl82/h2loader_app/src/jieli_app_iostreamikcp.c',
]

def function(source, name):
    prefix = 'static h2_pal_result_t ' if 'static h2_pal_result_t ' + name + '(' in source else 'static int '
    begin = source.index(prefix + name + '(')
    brace = source.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]

class CommandDeadlinesTest(unittest.TestCase):
    def test_providers(self):
        for path in SOURCES:
            source = (ROOT / path).read_text()
            kind = 'h2_jieli_app_transport' if 'h2_jieli_app_transport' in source else 'h2_jieli_transport'
            begin = source.index('typedef struct ' + kind + ' {')
            structure = source[begin:source.index('} ' + kind + '_t;', begin) + len('} ' + kind + '_t;')]
            activation = source[source.index('static int activate_pending('):]
            begin = activation.index('  const h2_iostreamikcp_config_t config = {')
            config = activation[begin:activation.index('  };', begin) + 4]
            config = config.replace('now_ms32', 'fixture_now').replace('.now_ms = now_ms,', '.now_ms = fixture_now,')
            functions = []
            for name in ['transport_read', 'transport_write', 'transport_flush', 'send_control', 'command_read_impl', 'command_read', 'command_write_impl', 'command_write', 'command_flush_impl', 'command_flush']:
                if 'static int ' + name + '(' in source or 'static h2_pal_result_t ' + name + '(' in source:
                    functions.append(function(source, name))
            fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_command_deadlines.c').read_text()
            fixture = fixture.replace('/* STRUCTURE */', structure).replace('TRANSPORT', kind + '_t')
            fixture = fixture.replace('/* FUNCTIONS */', '\n'.join(functions)).replace('/* CONFIG */', config)
            with tempfile.TemporaryDirectory() as directory:
                unit = Path(directory) / 'test.c'
                binary = Path(directory) / 'test'
                unit.write_text(fixture)
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                    '-I', str(ROOT / 'libs/pal/include'),
                    '-I', str(ROOT / 'libs/iostreamikcp/include'), str(unit), '-o', str(binary)], check=True)
                for case in ['finite', 'zero', 'window', 'zero_window', 'two_frames', 'read', 'control', 'flush', 'expired']:
                    with self.subTest(provider=path, case=case):
                        result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
