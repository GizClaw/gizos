"""Exercise successful and competing failed USB diagnostic lock calls."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TARGETS = (("native_component_src/jieli/wl82/h2_pal_image/src/h2_jieli_wl82_console.c", "static uint32_t ms_to_ticks", False),)


class UsbDebugLockTest(unittest.TestCase):
    def test_failed_contender_cannot_cancel_owner_release(self):
        for relative, end_marker, loader in TARGETS:
            with self.subTest(target=relative):
                source = (ROOT / relative).read_text()
                begin = source.index("int h2_jieli_usb_debug_try_lock(void)")
                end = source.index(end_marker, begin)
                stub = r'''
#include <assert.h>
#include "h2_atomic.h"
#define OS_NO_ERR 0
static int owner, caller=1, releases;
static int os_mutex_accept(int *mutex) {
 (void)mutex; if (owner) return -1; owner=caller; return 0;
}
static int os_mutex_post(int *mutex) {
 (void)mutex; assert(owner == caller); owner=0; ++releases;
 return 0;
}
'''
                if loader:
                    stub += "static int usb_tx_ready=1, usb_tx_mutex; static h2_atomic_bool_t usb_debug_locked;\n"
                else:
                    stub += "static struct { int started, tx_mutex; } state={1,0}; static volatile uint32_t debug_draining, debug_gate=3; enum { DEBUG_RAW_IDLE, DEBUG_RAW_BUSY, DEBUG_STARTING, DEBUG_READY };\nstatic void h2_jieli_atomic_store_u32(volatile uint32_t *p,uint32_t v) { *p=v; }\nstatic uint32_t h2_jieli_atomic_load_u32(const volatile uint32_t *p) { return *p; }\nstatic int h2_jieli_atomic_cas_u32(volatile uint32_t *p,uint32_t *expected,uint32_t next) { if(*p!=*expected) { *expected=*p; return 0; } *p=next;return 1; }\n"
                    if "static int usb_debug_lock_held;" in source:
                        stub += "static int usb_debug_lock_held;\n"
                main = r'''
int main(void) {
 INIT_ATOMIC
 debug_gate=DEBUG_RAW_IDLE;
 caller=1; assert(h2_jieli_usb_debug_try_lock()==1 && owner==0);
 caller=2; assert(h2_jieli_usb_debug_try_lock()==0 && h2_jieli_usb_debug_is_draining());
 caller=1; h2_jieli_usb_debug_unlock();
 assert(debug_gate==DEBUG_RAW_IDLE && owner==0 && releases==0 && !h2_jieli_usb_debug_is_draining());
 debug_gate=DEBUG_STARTING; assert(h2_jieli_usb_debug_try_lock()==0);
 debug_gate=DEBUG_READY;
 for (unsigned round=0; round<100; ++round) {
  caller=1; assert(h2_jieli_usb_debug_try_lock()==1);
  caller=2; assert(h2_jieli_usb_debug_try_lock()==0);
  CHECK_DRAINING
  caller=1; h2_jieli_usb_debug_unlock();
  assert(owner==0 && releases==(int)(round*2+1));
  CHECK_IDLE
  caller=2; assert(h2_jieli_usb_debug_try_lock()==1);
  h2_jieli_usb_debug_unlock();
  assert(owner==0 && releases==(int)(round*2+2));
 }
 DESTROY_ATOMIC
 return 0;
}
'''
                main = main.replace("INIT_ATOMIC", "assert(h2_atomic_bool_init(&usb_debug_locked, false)==H2_ATOMIC_OK);" if loader else "")
                main = main.replace("DESTROY_ATOMIC", "h2_atomic_bool_destroy(&usb_debug_locked);" if loader else "")
                main = main.replace("CHECK_DRAINING", "assert(h2_jieli_usb_debug_is_draining()==1);")
                main = main.replace("CHECK_IDLE", "assert(h2_jieli_usb_debug_is_draining()==0);")
                with tempfile.TemporaryDirectory(prefix="h2-usb-debug-lock-") as directory:
                    test = Path(directory) / "test.c"
                    test.write_text(stub + source[begin:end] + main)
                    binary = Path(directory) / "test"
                    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "libs/atomic/include"), str(test),
                                    str(ROOT / "libs/atomic/providers/c11/src/h2_atomic_c11.c"), "-o", str(binary)], check=True, timeout=60)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
