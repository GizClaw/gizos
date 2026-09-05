"""Exercise successful and competing failed USB diagnostic lock calls."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TARGETS = (
    ("projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit/src/jieli_app_iostreamikcp.c", "static uint32_t now_ms", False),
    ("projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit/src/loader_launcher.c", "static void usb_diag_write", True),
)


class UsbDebugLockTest(unittest.TestCase):
    def test_failed_contender_cannot_cancel_owner_release(self):
        for relative, end_marker, loader in TARGETS:
            with self.subTest(target=relative):
                source = (ROOT / relative).read_text()
                begin = source.index("int h2_jieli_usb_debug_try_lock(void)")
                end = source.index(end_marker, begin)
                stub = r'''
#include <assert.h>
#define OS_NO_ERR 0
static int owner, caller=1, releases;
static int os_mutex_accept(int *mutex) {
 (void)mutex; if (owner) return -1; owner=caller; return 0;
}
static int os_mutex_post(int *mutex) {
 (void)mutex; assert(owner == caller); owner=0; ++releases; return 0;
}
'''
                if loader:
                    stub += "static int usb_tx_ready=1, usb_tx_mutex, usb_debug_locked;\n"
                else:
                    stub += "static struct { int started, tx_mutex; } state={1,0};\n"
                    if "static int usb_debug_lock_held;" in source:
                        stub += "static int usb_debug_lock_held;\n"
                main = r'''
int main(void) {
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
 return 0;
}
'''
                main = main.replace("CHECK_DRAINING", "assert(h2_jieli_usb_debug_is_draining()==1);" if loader else "")
                main = main.replace("CHECK_IDLE", "assert(h2_jieli_usb_debug_is_draining()==0);" if loader else "")
                with tempfile.TemporaryDirectory(prefix="h2-usb-debug-lock-") as directory:
                    test = Path(directory) / "test.c"
                    test.write_text(stub + source[begin:end] + main)
                    binary = Path(directory) / "test"
                    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True, timeout=60)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
