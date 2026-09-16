"""Execute target entry/worker ownership with injected failures and pthread handoff."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
BASE = 'projects/example/targets/h2loader_tar_zlib'
PATHS = {name: f'{BASE}/{name}/jieli_ac791n_devkit/src/{file}.c' for name, file in (
    ('audio-system', 'audio_system_target'), ('button', 'button_target'),
    ('touch', 'touch_target'), ('crash-before-confirm', 'crash_before_confirm_target'),
    ('mp4-player', 'mp4_player_small_pal'))}
PATHS['pal'] = 'projects/e2e/targets/h2loader_tar_zlib/pal/jieli_ac791n_devkit/src/pal_e2e_main.c'


def source(path):
    baseline = os.environ.get('JIELI_ENTRY_BASELINE')
    return subprocess.check_output(['git', 'show', f'{baseline}:{path}'], text=True) if baseline else (ROOT / path).read_text()


def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class RuntimeLifetimeTest(unittest.TestCase):
    def test_entries(self):
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_target_runtime_lifetime.c').read_text()
        for name, path in PATHS.items():
            with self.subTest(target=name):
                text = source(path)
                worker = name in ('button', 'touch')
                if worker:
                    start = text.index(f'typedef struct {name}_target_state')
                    code = text[start:text.index('static void emit(', start)]
                    code += text[text.index('static int should_stop('):]
                elif name == 'mp4-player':
                    code = text[text.index('static void mp4_runtime('):text.index('  emit("H2_JIELI_MP4_FAIL')]
                    code = code.replace('static void mp4_runtime(void *user)', 'int h2_jieli_target_application_run(void)').replace('  (void)user;\n', '')
                    code += '  return result;\n}\n'
                elif name == 'pal':
                    start = text.index('  if (result == H2_PAL_OK) result = h2_runtime_init')
                    code = '''int h2_jieli_target_application_run(void) {
 h2_runtime_config_t config = {0}; h2_runtime_t *runtime = NULL;
 int result = h2_jieli_ac791n_devkit_runtime_config(&config);
 h2_pal_e2e_result_t reports[3] = {{0}};
 const uint32_t suites[3] = {1, 2, 3}; size_t passed = 0, failed = 0;
'''+text[start:text.index('  /* Deliberately leave this diagnostic App', start)]
                    code += ' (void)passed; (void)failed; return result;\n}\n'
                else:
                    code = function(text, 'int h2_jieli_target_application_run(')
                unit_text = fixture.replace('/* TARGET */', code).replace('WORKER_TARGET', '1' if worker else '0')
                unit_text = unit_text.replace('CRASH_TARGET', '1' if name == 'crash-before-confirm' else '0')
                unit_text = unit_text.replace('AUDIO_TARGET', '1' if name == 'audio-system' else '0')
                with tempfile.TemporaryDirectory() as directory:
                    unit = Path(directory) / 'test.c'; binary = Path(directory) / 'test'
                    unit.write_text(unit_text)
                    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')), str(unit), '-o', str(binary)], check=True)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    if name == 'audio-system':
                        self.assertEqual(result.stdout.count(
                            'cleanup did not complete; Runtime intentionally retained result=-16 attempts=100'), 1)


if __name__ == '__main__':
    unittest.main()
