"""Verify the wl82 __sync_* runtime that replaces the compiler-rt libcalls."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "native_component_src/jieli/wl82/h2_pal_core/src/h2_jieli_wl82_sdk_port.c"

# The SDK spinlock becomes a pthread mutex; the asm labels are dropped so the
# host compiler's own __sync builtins stay untouched.
STUB = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
typedef struct { pthread_mutex_t mutex; } spinlock_t;
#define rwlock mutex
#undef __asm__
#define __asm__(name)
static void spin_lock(spinlock_t *lock) { pthread_mutex_lock(&lock->mutex); }
static void spin_unlock(spinlock_t *lock) { pthread_mutex_unlock(&lock->mutex); }
'''

MAIN = r'''
#define THREADS 4
#define ROUNDS 20000
static volatile uint32_t counter;
static volatile uint32_t cas_counter;
static void *worker(void *user) {
  (void)user;
  for (int i = 0; i < ROUNDS; ++i) {
    h2_jieli_sync_fetch_and_add_4(&counter, 3u);
    h2_jieli_sync_fetch_and_sub_4(&counter, 1u);
    for (;;) {
      uint32_t seen = cas_counter;
      if (h2_jieli_sync_cas_4(&cas_counter, seen, seen + 1u) == seen) break;
    }
  }
  return 0;
}
int main(void) {
  volatile uint8_t byte = 0xf0u;
  assert(h2_jieli_sync_fetch_and_or_1(&byte, 0x0fu) == 0xf0u && byte == 0xffu);
  assert(h2_jieli_sync_fetch_and_and_1(&byte, 0x3cu) == 0xffu && byte == 0x3cu);
  assert(h2_jieli_sync_fetch_and_xor_1(&byte, 0xffu) == 0x3cu && byte == 0xc3u);
  volatile uint16_t half = 7u;
  assert(h2_jieli_sync_lock_test_and_set_2(&half, 9u) == 7u && half == 9u);
  assert(h2_jieli_sync_cas_2(&half, 8u, 1u) == 9u && half == 9u);
  assert(h2_jieli_sync_cas_2(&half, 9u, 1u) == 9u && half == 1u);
  volatile uint64_t wide = UINT64_C(0x100000000);
  assert(h2_jieli_sync_fetch_and_sub_8(&wide, 1u) == UINT64_C(0x100000000));
  assert(wide == UINT64_C(0xffffffff));
  /* The PAL lifecycle word: ACTIVE with no references must not wrap. */
  volatile uint32_t lifecycle = UINT32_C(0x80000000);
  assert(h2_jieli_sync_fetch_and_add_4(&lifecycle, 1u) == UINT32_C(0x80000000));
  assert(h2_jieli_sync_fetch_and_sub_4(&lifecycle, 1u) == UINT32_C(0x80000001));
  assert(lifecycle == UINT32_C(0x80000000));
  pthread_t threads[THREADS];
  for (int i = 0; i < THREADS; ++i) assert(pthread_create(&threads[i], 0, worker, 0) == 0);
  for (int i = 0; i < THREADS; ++i) assert(pthread_join(threads[i], 0) == 0);
  assert(counter == 2u * THREADS * ROUNDS);
  assert(cas_counter == (uint32_t)THREADS * ROUNDS);
  return 0;
}
'''


class SyncAtomicsTest(unittest.TestCase):
    def runtime_block(self):
        source = SOURCE.read_text()
        start = source.index("static spinlock_t h2_jieli_atomic_lock")
        return source[start:]

    def test_every_linked_libcall_is_overridden(self):
        """compiler-rt members linked into AC791N images must all be replaced."""
        block = self.runtime_block()
        widths = re.findall(r"H2_JIELI_SYNC_WIDTH\((\d), ", block)
        self.assertEqual(widths, ["1", "2", "4", "8"])
        for name in ("fetch_and_add", "fetch_and_sub", "lock_test_and_set"):
            self.assertIn(f"H2_JIELI_SYNC_RMW(width, type, {name},", block)
        self.assertIn('__asm__("__sync_" #name "_" #width)', block)
        self.assertIn('__asm__("__sync_val_compare_and_swap_" #width)', block)
        self.assertNotIn("lockset", block.split("*/", 1)[1])

    def test_operations_are_atomic_and_return_previous_values(self):
        block = self.runtime_block().replace(
            "{.rwlock = 0}", "{PTHREAD_MUTEX_INITIALIZER}")
        with tempfile.TemporaryDirectory(prefix="h2-wl82-sync-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(STUB + block + MAIN)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=gnu11", "-pthread", str(test), "-o",
                            str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
