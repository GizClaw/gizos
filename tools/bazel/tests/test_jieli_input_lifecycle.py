"""Real display/touch/button source with native lifetime and first-use faults."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class InputLifecycleTest(unittest.TestCase):
    def test_provider(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_input.c').read_text()
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        # The native ioctl ABI is 32-bit. Preserve pointer width in the host fake.
        source = source.replace('(uint32_t)value', '(uintptr_t)value')
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_input_lifecycle.c').read_text()
        completion = 'display_emi_send_complete(NULL)' if 'display_emi_send_complete(void *device)' in source else 'display_emi_send_complete()'
        fixture = fixture.replace('/* COMPLETE */', completion)
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            unit.write_text(fixture.replace('/* PROVIDER */', source))
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                '-I', str(ROOT / 'libs/pal/include'),
                '-I', str(ROOT / 'boards/jieli_ac791n_devkit/ac791n/include'),
                '-I', str(ROOT / 'native_component_src/jieli/wl82/h2_pal_core/include'),
                str(unit), '-I', str(ROOT / 'libs/atomic/include'), str(ROOT / 'libs/atomic/providers/c11/src/h2_atomic_c11.c'), '-o', str(binary)], check=True)
            for case in ['invalid_input', 'flush_failure', 'display_close_error', 'touch_close_error', 'rs_order', 'flush_lost_irq', 'flush_stale', 'open_pending', 'open_flush_error',
                         'iic_read_start', 'iic_read_stop', 'iic_write_start', 'iic_write_stop',
                         'adc_full', 'adc_gpio', 'draw_close', 'touch_close', 'adc_first_use']:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                    self.assertNotIn('WARNING: ThreadSanitizer', result.stderr)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
