"""Verify the diagnostic stop only arms for the first P1 erase from P2."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit/src/loader_copy_powercut.c"


class CopyPowercutTest(unittest.TestCase):
    def test_pause_is_one_shot_across_erased_p1_restart(self):
        program = r'''
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include "fixture.c"
static uint32_t base;
static uint8_t p1[32], p2[32];
static int read_error, watchdog_closed;
static jmp_buf stopped;
uint32_t boot_info_get_sfc_base_addr(void) { return base; }
int norflash_origin_read(uint8_t *out, uint32_t addr, uint32_t len) {
  assert(len == 32);
  if (read_error) return -1;
  assert(addr == H2_JIELI_BANK_1_SFC_BASE-32 || addr == H2_JIELI_BANK_2_SFC_BASE-32);
  memcpy(out, addr == H2_JIELI_BANK_1_SFC_BASE-32 ? p1 : p2, len);
  return (int)len;
}
void wdt_close(void) { watchdog_closed++; }
void os_time_dly(unsigned ticks) { assert(ticks == 100); longjmp(stopped, 1); }
int main(void) {
  base=H2_JIELI_BANK_1_SFC_BASE;
  h2_jieli_wl82_boot_probe(105); assert(!pause_on_erase);
  base=H2_JIELI_BANK_2_SFC_BASE;
  read_error=1;h2_jieli_wl82_boot_probe(105);assert(!pause_on_erase);
  read_error=0;h2_jieli_wl82_boot_probe(105);assert(pause_on_erase);
  h2_jieli_upgrade_erase_observer(H2_JIELI_BANK_1_SFC_BASE-32);
  assert(!watchdog_closed); /* not physically erased yet */
  memset(p1,0xff,32);memset(p2,0xff,32);
  h2_jieli_upgrade_erase_observer(H2_JIELI_BANK_1_SFC_BASE-32);
  assert(!watchdog_closed); /* no published recovery header */
  memset(p2,0x5a,32);
  if (!setjmp(stopped)) {
    h2_jieli_upgrade_erase_observer(H2_JIELI_BANK_1_SFC_BASE-32);
    assert(0);
  }
  assert(watchdog_closed==1 && !pause_on_erase);
  h2_jieli_wl82_boot_probe(105);assert(!pause_on_erase);
  h2_jieli_upgrade_erase_observer(H2_JIELI_BANK_1_SFC_BASE-32);
  assert(watchdog_closed==1); /* reboot with erased P1 must continue */
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "asm").mkdir()
            (root / "os").mkdir()
            (root / "asm/sfc_norflash_api.h").write_text(
                "#include <stdint.h>\nint norflash_origin_read(uint8_t *,uint32_t,uint32_t);\n")
            (root / "asm/wdt.h").write_text("void wdt_close(void);\n")
            (root / "os/os_api.h").write_text("void os_time_dly(unsigned);\n")
            (root / "fixture.c").write_text(SOURCE.read_text())
            (root / "test.c").write_text(program)
            binary = root / "test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(root), "-I",
                str(ROOT / "boards/jieli_ac791n_devkit/ac791n/include"),
                str(root / "test.c"), "-o", str(binary),
            ], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)
