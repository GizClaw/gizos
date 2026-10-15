"""Execute the DevKit entry's initialization chain before its BLE scan loop."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = 'projects/example/targets/h2loader_tar_zlib/ble-connect-smoke/devkit/main/main.c'

FIXTURE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define H2_PAL_OK 0
typedef struct { int unused; } h2_runtime_config_t;
typedef struct { void *ble_host; } h2_runtime_t;
static h2_runtime_t instance;
static int calls, fail_at;
static int step(int expected) { assert(++calls == expected); return calls == fail_at ? -77 : 0; }
int h2_esp_board_runtime_config(h2_runtime_config_t *c) { assert(c); return step(1); }
int h2_esp_h2loader_app_commands_prepare_serial(h2_runtime_config_t *c, const char *n, unsigned a, unsigned b) {
    assert(c && strcmp(n, "ble-connect-smoke") == 0 && a == 1 && b == 3); return step(2);
}
int h2_runtime_init(h2_runtime_config_t *c, h2_runtime_t **r) { assert(c); *r = &instance; return step(3); }
int h2_esp_h2loader_app_commands_start(h2_runtime_t *r, const char *n, unsigned a, unsigned b) {
    assert(r == &instance && strcmp(n, "ble-connect-smoke") == 0 && a == 1 && b == 3); return step(4);
}
int h2_esp_platform_confirm_running_app(void) { return step(5); }
int h2_esp_h2loader_app_confirm(h2_runtime_t *r) { assert(r == &instance); return step(6); }
int h2_pal_ble_start(void *host) { assert(host == instance.ble_host); return step(7); }
/* ENTRY */
int main(void) {
    for (fail_at = 0; fail_at <= 7; ++fail_at) {
        calls = 0;
        assert(image_entry(NULL) == (fail_at ? -77 : 0));
        assert(calls == (fail_at ? fail_at : 7));
    }
    return 0;
}
'''


class StartupTest(unittest.TestCase):
    def test_start_before_confirmation_and_propagate_failure(self):
        source = (ROOT / SOURCE).read_text()
        entry = source[source.index('static void image_entry('):source.index('    for (unsigned attempt')]
        entry = entry.replace('static void image_entry(', 'static int image_entry(')
        entry += '    return rc;\n}\n'
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'test.c'
            binary = Path(directory) / 'test'
            unit.write_text(FIXTURE.replace('/* ENTRY */', entry))
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
