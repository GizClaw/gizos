"""Deadline and DMA-lifetime faults against the real console implementation."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class ConsoleDeadlinesTest(unittest.TestCase):
    def test_uart(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_console.c').read_text()
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        platform = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/src/platform.c').read_text()
        uart = platform[platform.index('UART1_PLATFORM_DATA_BEGIN'):platform.index('UART1_PLATFORM_DATA_END')]
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_console_deadlines.c').read_text()
        fixture = fixture.replace('/* ASYNC_MODE */', '1' if '.disable_tx_irq = 1' in uart else '0')
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            unit.write_text(fixture.replace('/* REAL_PROVIDER */', source))
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                '-I', str(ROOT / 'libs/pal/include'), str(unit), '-o', str(binary)], check=True)
            for case in ['read_busy', 'zero', 'deadline', 'dma_lifetime', 'short_write', 'stalled_dma', 'threaded_read']:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                    self.assertNotIn('WARNING: ThreadSanitizer', result.stderr)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
