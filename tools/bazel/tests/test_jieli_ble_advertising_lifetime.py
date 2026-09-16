"""Exercise real advertising publication and delayed SDK pointer consumers."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

def function(source, signature):
    begin = source.index(signature)
    brace = source.index('{', begin)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end] + '\n'

class AdvertisingLifetimeTest(unittest.TestCase):
    def test_advertising(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c').read_text()
        begin = source.index('struct h2_pal_ble_adv_set {')
        types = source[begin:source.index('typedef struct h2_jieli_ble_state', begin)]
        begin = source.index('struct h2_ext_adv_param {')
        types += source[begin:source.index('static void h2_ble_post(', begin)]
        begin = source.index('static int h2_adv_append(')
        code = source[begin:source.index('static int h2_ble_start(', begin)]
        begin = source.index('static int h2_adv_set_create(')
        code += source[begin:source.index('static int h2_register_gatt(', begin)]
        code += function(source, 'static int h2_command_rearm(void) {')
        code += function(source, 'static void h2_connection_command_consumed(void) {')
        code += function(source, 'static void h2_restart_legacy_advertising(void) {')
        code += function(source, 'static int h2_update_connection(')
        code += function(source, 'static int h2_ble_stop(')
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_ble_advertising_lifetime.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(fixture.replace('/* REAL_TYPES */', types).replace('/* REAL_PROVIDER */', code))
            binary = Path(directory) / 'test'
            flags = os.environ.get('JIELI_TEST_CFLAGS', '-fsanitize=address').split()
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', *flags,
                '-I', str(ROOT / 'libs/pal/include'), str(test), '-o', str(binary)], check=True)
            for case in ['shutdown_adv', 'shutdown_conn', 'rearm_natural', 'rearm_inline', 'rearm_apply_once', 'rearm_apply_persistent', 'rearm_stop_once', 'rearm_stop_persistent', 'rearm_restart_once', 'rearm_restart_persistent', 'early_hook', 'early_restart', 'stop_hook', 'extended', 'legacy_borrow', 'failed_update', 'null_uuids', 'submitting', 'state_race', 'registration_error', 'fence_error', 'stop_error', 'deferred', 'pending_init']:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=15,
                        env=dict(os.environ, ASAN_OPTIONS='detect_stack_use_after_return=1'))
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
