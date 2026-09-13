"""Threaded execution of the actual BLE host start/stop/INIT lifecycle."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

def function(source, signature):
    begin = source.index(signature)
    brace = source.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end] + '\n'

class HostLifecycleTest(unittest.TestCase):
    def test_extended_disable_storage(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c').read_text()
        begin = source.index('struct h2_ext_adv_enable {')
        end = source.index('} __attribute__((packed));', begin) + len('} __attribute__((packed));')
        code = source[begin:end] + '\n' + function(source, 'static int h2_adv_set_stop(void *user, h2_pal_ble_adv_set_t *set) {')
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_ble_disable_storage.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(fixture.replace('/* REAL_PROVIDER */', code))
            binary = Path(directory) / 'test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address',
                '-I', str(ROOT / 'libs/pal/include'), str(test), '-o', str(binary)], check=True)
            env = dict(os.environ, ASAN_OPTIONS='detect_stack_use_after_return=1')
            result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_lifecycle(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c').read_text()
        if '/* Host lifetime helpers. */' in source:
            helpers = source.split('/* Host lifetime helpers. */', 1)[1].split('/* End host lifetime helpers. */', 1)[0]
            retained = function(source, 'static int h2_ble_start_retained(')
            retained += function(source, 'static int h2_notify_retained(')
            retained += function(source, 'static int h2_att_write_retained(')
        else:
            helpers = '''typedef struct { int unused; } h2_ble_call_t;
static int h2_ble_call_begin(h2_ble_call_t *call) { (void)call; return H2_PAL_OK; }
static void h2_ble_call_end(h2_ble_call_t *call) { (void)call; }
'''
            retained = '''static int h2_ble_start_retained(void *user) { return h2_ble_start(user); }
static int h2_notify_retained(void *u, uint16_t c, uint16_t a, const uint8_t *d, size_t n) { return h2_notify(u,c,a,d,n); }
static int h2_att_write_retained(uint16_t c, uint16_t a, uint16_t t, uint16_t o, uint8_t *d, uint16_t n) { return h2_att_write(c,a,t,o,d,n); }
'''
        code = helpers + function(source, 'static int h2_ble_start(')
        code += function(source, 'static int h2_ble_stop(')
        code += function(source, 'void bt_ble_init(void)') + retained
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_ble_host_lifecycle.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(fixture.replace('/* REAL_PROVIDER */', code))
            binary = Path(directory) / 'test'
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-pthread',
                *os.environ.get('JIELI_TEST_CFLAGS', '').split(), '-I', str(ROOT / 'libs/pal/include'),
                str(test), '-o', str(binary)], check=True)
            for case in ['adv_error', 'disconnect_error', 'exit_error', 'retained', 'retained_callback', 'self_stop', 'pending_init', 'late_init', 'init_dispatcher', 'admitted_start', 'init_error', 'init_publication', 'disconnect_event']:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
