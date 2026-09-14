"""Exercise actual Wi-Fi SDK events and cached readers with real pthreads."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class WifiSnapshotsTest(unittest.TestCase):
    def test_snapshots(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_wifi.c').read_text()
        state = source[source.index('typedef struct h2_jieli_wifi_state'):source.index('enum { SCAN_IDLE')]
        posts = source[source.index('static void post_sta_event('):source.index('static uint32_t pack_ip4(')]
        event = source[source.index('static int wifi_event('):source.index('static int ensure_wifi_on(')]
        sta = source[source.index('static int sta_get_status('):source.index('static int sta_scan(')]
        ap = source[source.index('static int ap_get_status('):source.index('static int ap_get_clients(')]
        clients = source[source.index('static int ap_get_clients('):source.index('/* Serialize task-side')]
        admission = source[source.index('static unsigned wifi_operation_busy;'):source.index('static int guarded_sta_scan(')]
        fixture = (ROOT / 'tools/bazel/tests/fixtures/jieli_wifi_snapshots.c').read_text()
        if 'update_sta_snapshot(h2_pal_wifi_sta_status_t *status)' in state:
            fixture = fixture.replace('static void update_sta_snapshot(void) { fake_sdk_refresh(); }',
                'static void update_sta_snapshot(h2_pal_wifi_sta_status_t *status) { status->ip_valid = !refresh_error; fake_sdk_refresh(); }')
        if 'wifi_state_gate' in state:
            # Instrument ownership without replacing the real lock algorithm.
            state = state.replace('static void wifi_state_lock(void)',
                'static _Thread_local unsigned gate_owned;\nstatic void wifi_state_lock(void)')
            state = state.replace('if (h2_jieli_atomic_cas_u32(&wifi_state_gate, &expected, 1u)) return;',
                'if (h2_jieli_atomic_cas_u32(&wifi_state_gate, &expected, 1u)) {\n'
                '      assert(gate_owned == 0);\n      gate_owned = 1;\n      return;\n    }')
            state = state.replace('  h2_jieli_atomic_store_u32(&wifi_state_gate, 0u);',
                '  assert(gate_owned == 1);\n  gate_owned = 0;\n'
                '  h2_jieli_atomic_store_u32(&wifi_state_gate, 0u);')
        fixture = fixture.replace('/* SDK_GATE_CHECK */',
            'assert(gate_owned == 0);' if 'wifi_state_gate' in state else '')
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            unit.write_text(fixture.replace('/* REAL_PROVIDER */', state + posts + event + sta + ap + clients + admission))
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                '-I', str(ROOT / 'libs/pal/include'),
                '-I', str(ROOT / 'native_component_src/jieli/wl82/h2_pal_core/include'),
                str(unit), '-o', str(binary)], check=True)
            for case in ['payload', 'readers', 'stale_refresh', 'ip_failure', 'ap_clients']:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=15)
                    self.assertNotIn("WARNING: ThreadSanitizer", result.stderr, result.stdout + result.stderr)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
