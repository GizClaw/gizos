"""Exercise the actual WL82 allocation wrapper, not the PAL host fake."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "native_component_src/jieli/wl82/h2_pal_core/src/h2_jieli_wl82_sdk_port.c"


class SdkMemoryTest(unittest.TestCase):
    def test_realloc_failure_preserves_original_allocation(self):
        source = SOURCE.read_text()
        begin = source.index("#define H2_JIELI_WL82_MEM_HEADER")
        end = source.index("/* ---- Layout-selected debug output", begin)
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static int live, fail_alloc;
static void *test_malloc(size_t size) {
    if (fail_alloc) return NULL;
    void *p = malloc(size);
    if (p) ++live;
    return p;
}
static void test_free(void *p) {
    if (p) --live;
    free(p);
}
void h2_jieli_sdk_free(void *ptr);
#define malloc test_malloc
#define free test_free
'''
        main = r'''
int main(void) {
    assert(h2_jieli_sdk_malloc(0) == NULL);
    assert(h2_jieli_sdk_malloc(SIZE_MAX) == NULL);
    unsigned char *p = h2_jieli_sdk_malloc(32);
    assert(p && live == 1);
    memset(p, 0x5a, 32);
    fail_alloc = 1;
    assert(h2_jieli_sdk_realloc(p, 64) == NULL);
    assert(live == 1);
    for (int i = 0; i < 32; ++i) assert(p[i] == 0x5a);
    assert(h2_jieli_sdk_realloc(p, 16) == p);
    fail_alloc = 0;
    unsigned char *q = h2_jieli_sdk_realloc(p, 64);
    assert(q && live == 1);
    for (int i = 0; i < 16; ++i) assert(q[i] == 0x5a);
    assert(h2_jieli_sdk_realloc(q, 0) == NULL && live == 0);
    q = h2_jieli_sdk_realloc(NULL, 8);
    assert(q && live == 1);
    h2_jieli_sdk_free(q);
    h2_jieli_sdk_free(NULL);
    assert(live == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-sdk-memory-") as directory:
            root = Path(directory)
            unit = root / "memory.c"
            unit.write_text(fixture + source[begin:end] + main)
            binary = root / "memory-test"
            subprocess.run(["cc", "-std=c11", str(unit), "-o", str(binary)],
                           check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
