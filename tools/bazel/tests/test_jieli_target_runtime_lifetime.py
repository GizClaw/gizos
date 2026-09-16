"""Execute the AC791N PAL E2E entry against injected Runtime and suite failures.

The real `run_suites()` body is spliced into a host fixture that counts
`h2_runtime_init`/`h2_runtime_deinit` and owns the retained-cleanup token. The
fixture asserts `inits == deinits`, no live Runtime and no retained cleanup after
every injected fault, so the entry must drain deferred cleanup and then release
the Runtime before it reaches its ledger loop.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PAL_PATH = 'projects/e2e/targets/h2loader_tar_zlib/pal/jieli_ac791n_devkit/src/pal_e2e_main.c'


def source(path):
    baseline = os.environ.get('JIELI_ENTRY_BASELINE')
    return subprocess.check_output(['git', 'show', f'{baseline}:{path}'], text=True) if baseline else (ROOT / path).read_text()


class RuntimeLifetimeTest(unittest.TestCase):
    def test_pal_entry(self):
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_target_runtime_lifetime.c').read_text()
        text = source(PAL_PATH)
        start = text.index('  if (result == H2_PAL_OK) result = h2_runtime_init')
        code = '''int h2_jieli_target_application_run(void) {
 h2_runtime_config_t config = {0}; h2_runtime_t *runtime = NULL;
 int result = h2_jieli_ac791n_devkit_runtime_config(&config);
 h2_pal_e2e_result_t reports[3] = {{0}};
 const uint32_t suites[3] = {1, 2, 3}; size_t passed = 0, failed = 0;
'''+text[start:text.index('  /* Deliberately leave this diagnostic App', start)]
        code += ' (void)passed; (void)failed; return result;\n}\n'
        self.assertEqual(fixture.count('/* TARGET */'), 1)
        unit_text = fixture.replace('/* TARGET */', code)
        # This entry owns no worker task and is neither the crash nor the audio
        # scene, so the fixture's retained-Runtime branches stay compiled out.
        for marker in ('WORKER_TARGET', 'CRASH_TARGET', 'AUDIO_TARGET'):
            unit_text = unit_text.replace(marker, '0')
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            unit.write_text(unit_text)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')), str(unit), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
