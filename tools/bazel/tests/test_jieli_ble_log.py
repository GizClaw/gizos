"""Exercise the BLE provider's borrowed PAL diagnostic sink."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class BleLogTest(unittest.TestCase):
    def test_log_binding_framing_and_bounds(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c").read_text()
        self.assertNotRegex(source, r"\bprintf\s*\(")
        begin = source.index("static const h2_pal_log_api_t *h2_ble_log_api;")
        end = source.index("/* Required by JieLi", begin)
        fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "h2/pal/os/h2_pal_log.h"
static unsigned calls;
static char captured[H2_PAL_LOG_MESSAGE_MAX + 1];
static int sink(void *user, h2_pal_log_level_t level, const char *scope, const char *message) {
 assert(user == &calls && level == H2_PAL_LOG_DEBUG);
 assert(strcmp(scope, "jieli/ble") == 0);
 assert(strlen(message) <= H2_PAL_LOG_MESSAGE_MAX);
 strcpy(captured, message);
 ++calls;
 return H2_PAL_OK;
}
'''
        main = r'''
int main(void) {
 h2_ble_log("before binding");
 assert(calls == 0);
 const h2_pal_log_vtable_t vtable = {.write=sink};
 const h2_pal_log_api_t log = {.user=&calls, .vtable=&vtable};
 const h2_pal_log_api_t replacement = {.user=&calls, .vtable=&vtable};
 const h2_pal_log_api_t invalid = {0};
 assert(!h2_ble_log_bind(NULL) && !h2_ble_log_bind(&invalid));
 assert(h2_ble_log_bind(&log) && h2_ble_log_bind(&log));
 assert(!h2_ble_log_bind(&replacement));
 h2_ble_log("value=%d %s\r\n", 7, "%n");
 assert(calls == 1 && strcmp(captured, "value=7 %n") == 0);
 char large[600]; memset(large, 'x', sizeof(large)-1); large[599]=0;
 h2_ble_log("%s", large);
 assert(calls == 2 && strlen(captured) == H2_PAL_LOG_MESSAGE_MAX);
 h2_ble_log("\r\n");
 assert(calls == 3 && captured[0] == 0);
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-ble-log-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(fixture + source[begin:end] + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(ROOT / "libs/pal/include"),
                            str(test), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
