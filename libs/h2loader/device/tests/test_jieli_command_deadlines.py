"""Command budgets reach real Loader/App KCP physical I/O wiring."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
SOURCES = ['libs/h2loader/device/src/h2_device_serial.c']

def function(source, name):
    prefix = next(p for p in ('static h2_pal_result_t ', 'static int ', 'static void ', 'static uint32_t ') if p + name + '(' in source)
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
            kind = 'h2_device_serial'
            header = (ROOT / 'libs/h2loader/device/include/h2_device_serial.h').read_text()
            structure = '#include "h2_atomic.h"\ntypedef void h2_pal_time_api_t; typedef int h2_command_io_api_t;\n'
            structure += header[header.index('typedef struct h2_device_serial_config {'):header.index('h2_pal_result_t h2_device_serial_init_with_config(')]
            structure += '#define h2_device_serial_now(self) timer_get_ms()\n#define transport_stopped(self) stop_requested\n'
            activation = source[source.index('h2_pal_result_t h2_device_serial_activate_pending('):]
            begin = activation.index('  const h2_iostreamikcp_config_t config = {')
            config = activation[begin:activation.index('  };', begin) + 4]
            config = config.replace('now_ms32', 'fixture_now').replace('.now_ms = now_ms,', '.now_ms = fixture_now,')
            functions = []
            for name in ['deadline_start', 'remaining_ms', 'transport_read', 'transport_write', 'transport_flush', 'send_control', 'command_read_impl', 'command_read', 'command_write_impl', 'command_write', 'flush_budget', 'command_flush']:
                if any(prefix + name + '(' in source for prefix in ('static int ', 'static h2_pal_result_t ', 'static void ', 'static uint32_t ')):
                    functions.append(function(source, name))
            fixture = (ROOT / 'libs/h2loader/device/tests/fixtures/jieli_command_deadlines.c').read_text()
            fixture = fixture.replace('/* STRUCTURE */', structure).replace('TRANSPORT', kind + '_t')
            fixture = fixture.replace('/* FUNCTIONS */', '\n'.join(functions)).replace('/* CONFIG */', config)
            with tempfile.TemporaryDirectory() as directory:
                unit = Path(directory) / 'test.c'
                binary = Path(directory) / 'test'
                unit.write_text(fixture)
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                    '-I', str(ROOT / 'pal/include'), '-I', str(ROOT / 'libs/sync/atomic/include'),
                    '-I', str(ROOT / 'libs/net/iostreamikcp/include'), str(unit), '-o', str(binary)], check=True)
                for case in ['finite', 'zero', 'window', 'zero_window', 'two_frames', 'read', 'control', 'flush', 'expired']:
                    with self.subTest(provider=path, case=case):
                        result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
