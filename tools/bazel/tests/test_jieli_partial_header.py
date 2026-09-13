"""Exercise the test-only P2 header interruption fixture's safety guards."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit/src/loader_partial_header.c"


class PartialHeaderTest(unittest.TestCase):
    def test_only_writes_p2_with_valid_p1(self):
        program = r'''
#include <assert.h>
#include <setjmp.h>
#include "fixture.c"
static uint32_t base;
static uint8_t p1[32], p2[32];
static int writes, closed, read_error;
static jmp_buf stopped;
uint32_t boot_info_get_sfc_base_addr(void) { return base; }
int decode_data_by_user_key(uint16_t k,uint8_t *b,uint16_t n,uint32_t a,uint8_t s) {
  (void)b; assert(k==0xffff && n==32 && a==0 && s==32); return 0;
}
int norflash_origin_read(uint8_t *out,uint32_t addr,uint32_t len) {
  if (read_error) return -1;
  assert(len==32);
  assert(addr==H2_JIELI_BANK_1_SFC_BASE-32 || addr==H2_JIELI_BANK_2_SFC_BASE-32);
  memcpy(out,addr==H2_JIELI_BANK_1_SFC_BASE-32 ? p1 : p2,len); return len;
}
int norflash_protect_suspend(void) { return 0; }
int norflash_protect_resume(void) { return 0; }
int norflash_write(void *unused,const uint8_t *in,uint32_t len,uint32_t addr) {
  assert(!unused && len==16 && addr==H2_JIELI_BANK_2_SFC_BASE-32);
  memcpy(p2,in,len); writes++; return len;
}
void wdt_close(void) { closed++; }
void os_time_dly(unsigned ticks) { assert(ticks==100); longjmp(stopped,1); }
int main(void) {
  uint8_t header[32]; memset(header,0x5a,32); memset(p2,0xff,32);
  base=H2_JIELI_BANK_1_SFC_BASE;
  h2_jieli_upgrade_publish_observer(header); assert(!writes);
  base=H2_JIELI_BANK_2_SFC_BASE; read_error=1;
  h2_jieli_upgrade_publish_observer(header); assert(!writes);
  read_error=0; p1[0]=1;
  h2_jieli_upgrade_publish_observer(header); assert(!writes);
  p1[0]=0; /* all-zero mock decoded P1 has valid CRC */
  if (!setjmp(stopped)) { h2_jieli_upgrade_publish_observer(header); assert(0); }
  assert(writes==1 && closed==1);
  assert(memcmp(p2,header,16)==0);
  for (int i=16;i<32;i++) assert(p2[i]==0xff);
  for (int i=0;i<32;i++) assert(p1[i]==0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "asm").mkdir()
            (root / "os").mkdir()
            (root / "asm/sfc_norflash_api.h").write_text(
                "#include <stdint.h>\nint norflash_origin_read(uint8_t *,uint32_t,uint32_t);\n"
                "int norflash_write(void *,const uint8_t *,uint32_t,uint32_t);\n"
                "int norflash_protect_suspend(void);\nint norflash_protect_resume(void);\n")
            (root / "asm/wdt.h").write_text("void wdt_close(void);\n")
            (root / "os/os_api.h").write_text("void os_time_dly(unsigned);\n")
            (root / "fixture.c").write_text(SOURCE.read_text())
            (root / "test.c").write_text(program)
            binary = root / "test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(root), "-I", str(ROOT / "boards/jieli_ac791n_devkit/ac791n/include"),
                "-I", str(ROOT / "boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/include"),
                str(root / "test.c"), "-o", str(binary),
            ], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)
