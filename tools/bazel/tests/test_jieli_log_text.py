"""Check the native PAL debug adapter sends bounded text, not a buffer dump."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class LogTextTest(unittest.TestCase):
    def test_length_delimited_log_uses_one_text_write(self):
        source = (ROOT / "native_component_src/jieli/wl82/h2_pal_core/src/h2_jieli_wl82_sdk_port.c").read_text()
        begin = source.index("void h2_jieli_sdk_debug_write(")
        end = source.index("/* ---- Time", begin)
        fixture = r'''
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#define H2_JIELI_WL82_LOG_LINE_MAX 320u
static char output[1024];
static size_t used;
static unsigned writes;
static int text_printf(const char *format, ...) {
    assert(strcmp(format, "%s") == 0);
    va_list args;
    va_start(args, format);
    const char *text = va_arg(args, const char *);
    size_t length = strlen(text);
    assert(used + length <= sizeof(output));
    memcpy(output + used, text, length);
    used += length;
    ++writes;
    va_end(args);
    return (int)length;
}
#define printf text_printf
'''
        main = r'''
int main(void) {
    const char line[] = {'[','E',']',' ','%','n','\r','\n'};
    h2_jieli_sdk_debug_write(NULL, 12);
    h2_jieli_sdk_debug_write(line, 0);
    assert(writes == 0);
    h2_jieli_sdk_debug_write(line, sizeof(line));
    assert(writes == 1 && used == sizeof(line));
    assert(memcmp(output, line, sizeof(line)) == 0);
    char maximum[H2_JIELI_WL82_LOG_LINE_MAX];
    memset(maximum, 'x', sizeof(maximum));
    used = writes = 0;
    h2_jieli_sdk_debug_write(maximum, sizeof(maximum));
    assert(writes == 1 && used == sizeof(maximum));
    assert(memcmp(output, maximum, sizeof(maximum)) == 0);
    char large[H2_JIELI_WL82_LOG_LINE_MAX + 1];
    memset(large, 'z', sizeof(large));
    used = writes = 0;
    h2_jieli_sdk_debug_write(large, sizeof(large));
    assert(writes == 2 && used == sizeof(large));
    assert(memcmp(output, large, sizeof(large)) == 0);
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-jieli-log-text-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(fixture + source[begin:end] + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", str(test), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
