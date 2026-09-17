"""Compile the real board audio provider against a threaded audio-server fake."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class AudioLifecycleTest(unittest.TestCase):
    def test_lifecycle(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_audio.c').read_text()
        source = source[source.index('enum {'):source.rindex('\n#else')]
        source = re.sub(r'^#include .*\n', '', source, flags=re.M)
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_audio_lifecycle.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(fixture.replace('/* REAL_PROVIDER */', source))
            binary = Path(directory) / 'test'
            flags = os.environ.get('JIELI_TEST_CFLAGS', '').split()
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-pthread', *flags,
                '-I', str(ROOT / 'libs/pal/include'), '-I', str(ROOT / 'native_component_src/jieli/wl82/h2_pal_core/include'),
                str(test), '-o', str(binary)], check=True)
            for case in ['cycles', 'cycle_blocked_write', 'cycle_stale_callback', 'write', 'drain', 'drain_target', 'close', 'close_decoder', 'mic_generation', 'mic_tokens', 'mic_threads', 'sdk_failure', 'create_failure_open', 'create_failure_start']:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
