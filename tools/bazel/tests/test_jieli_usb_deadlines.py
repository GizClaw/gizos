"""Real Loader/App USB writers and the applied CDC patch, with native faults."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PATHS = [
    'projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit/src/loader_launcher.c',
    'projects/h2loader/native_component_src/jieli/wl82/h2loader_app/src/jieli_app_iostreamikcp.c',
]
PATCH = 'boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/sdk_patches/bounded_cdc_write.patch'

def function(source, signature):
    begin = source.index(signature)
    end = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]

class UsbDeadlinesTest(unittest.TestCase):
    def test_writers(self):
        applied = 'bounded_cdc_write.patch' in (ROOT / 'projects/h2loader/tools/bazel/h2loader_firmware.bzl').read_text()
        lines = (ROOT / PATCH).read_text().splitlines()
        sdk = '\n'.join(line[1:] for line in lines if line and line[0] in (' ', '+' if applied else '-') and not line.startswith(('+++', '---')))
        sdk = function(sdk, 'u32 cdc_write_data(')
        for path in PATHS:
            source = (ROOT / path).read_text()
            app = 'h2_jieli_app_console_t' in source
            fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_usb_deadlines.c').read_text()
            fixture = fixture.replace('/* SDK_WRITE */', sdk)
            fixture = fixture.replace('/* TICKS */', function(source, 'static uint32_t ms_to_ticks('))
            fixture = fixture.replace('/* PHYSICAL_WRITE */', function(source, 'static int physical_write('))
            fixture = fixture.replace('/* OUTER_MUTEX */', '&app.tx_mutex' if app else '&usb_tx_mutex')
            fixture = fixture.replace('/* USER */', '&app' if app else 'NULL')
            with tempfile.TemporaryDirectory() as directory:
                unit = Path(directory) / 'test.c'
                binary = Path(directory) / 'test'
                unit.write_text(fixture)
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                    '-I', str(ROOT / 'libs/pal/include'), str(unit), '-o', str(binary)], check=True)
                for case in ['overflow', 'zero_lock', 'lock', 'native_busy', 'offline', 'dma_missing', 'native_lock', 'packet', 'short']:
                    with self.subTest(provider=path, case=case):
                        result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                        self.assertNotIn('WARNING: ThreadSanitizer', result.stderr)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
