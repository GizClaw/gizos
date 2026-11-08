"""The App transport defers SESSION_OPEN until the confirmation console has drained."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
SOURCE = 'libs/h2loader/device/src/h2_device_serial.c'
CASES = ['gated', 'admit', 'fallback', 'reack', 'invalid']


def function(source, name):
    for prefix in ('static h2_pal_result_t ', 'static int ', 'static void ', 'static uint32_t '):
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
        header = (ROOT / 'libs/h2loader/device/include/h2_device_serial.h').read_text()
        structure = '#include "h2_atomic.h"\ntypedef void h2_pal_time_api_t; typedef int h2_command_io_api_t;\n'
        structure += header[header.index('typedef struct h2_device_serial_config {'):header.index('h2_pal_result_t h2_device_serial_init_with_config(')]
        structure += 'static h2_atomic_bool_t admission;\n#define h2_device_serial_now(self) timer_get_ms()\n#define transport_stopped(self) 0\n'
        functions = '\n'.join(function(source, name) for name in
                              ['write_le32', 'remaining_ms', 'transport_write', 'transport_flush', 'send_control', 'on_frame'])
        fixture = (ROOT / 'libs/h2loader/device/tests/fixtures/jieli_session_gate.c').read_text()
        fixture = fixture.replace('/* STRUCTURE */', structure).replace('TRANSPORT', 'h2_device_serial_t')
        fixture = fixture.replace('/* FUNCTIONS */', functions).replace('/* FALLBACK_MS */', '8000u')
        fixture = fixture.replace('  self->physical_io.write = fake_write;', '  self->physical_io.write = fake_write; self->config.sessions_admitted = &admission; self->config.admission_fallback_ms=8000u;')
        fixture = fixture.replace('  TRANSPORT transport;', '  TRANSPORT transport;')
        fixture = fixture.replace('  h2_device_serial_t transport;', '  h2_device_serial_t transport; assert(h2_atomic_bool_init(&admission,false)==H2_ATOMIC_OK);')
        fixture = fixture.replace('transport.sessions_admitted = 1;', 'h2_atomic_bool_store(&admission,true,H2_ATOMIC_RELEASE);')
        fixture = fixture.replace('transport.sessions_admitted ==', 'h2_atomic_bool_load(&admission,H2_ATOMIC_ACQUIRE) ==')
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            unit.write_text(fixture)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                            '-I', str(ROOT / 'pal/include'), '-I', str(ROOT / 'libs/sync/atomic/include'), str(ROOT / 'libs/sync/atomic/providers/c11/src/h2_atomic_c11.c'),
                            '-I', str(ROOT / 'libs/net/iostreamikcp/include'),
                            '-I', str(ROOT / 'native_component_src/jieli/wl82/h2_pal_core/include'),
                            str(unit), '-o', str(binary)], check=True)
            for case in CASES:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
