"""Exercise the layout-owned NOR upgrade adapter against a fake SDK driver."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/src/jieli_upgrade_io.c"


class UpgradeIoTest(unittest.TestCase):
    def test_nor_adapter_preserves_sdk_calls_and_results(self):
        declarations = r'''
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
struct device;
int norflash_read(struct device *,void *,u32,u32);
int norflash_origin_read(u8 *,u32,u32);
int norflash_write(struct device *,void *,u32,u32);
int norflash_ioctl(struct device *,u32,u32);
int norflash_protect_suspend(void);
int norflash_protect_resume(void);
'''
        program = r'''
#include <assert.h>
#include "adapter.c"
static int result, calls, suspended, resumed;
static u32 command, address, length;
static void *buffer;
static u32 base=0x4020;
static int emulate_flash, physical_writes;
static unsigned programmed_bytes=32;
static u8 flash_header[32];
u32 boot_info_get_sfc_base_addr(void) { return base; }
int norflash_read(struct device *dev,void *buf,u32 len,u32 addr) {
  assert(dev==NULL);calls++;buffer=buf;length=len;address=addr;return result;
}
int norflash_write(struct device *dev,void *buf,u32 len,u32 addr) {
  if (emulate_flash) {
    assert(addr==HEADER_ADDR && len==32);physical_writes++;
    if(result==32) memcpy(flash_header,buf,programmed_bytes);
  }
  return norflash_read(dev,buf,len,addr);
}
int norflash_origin_read(u8 *buf,u32 addr,u32 len) {
  if(emulate_flash && result==32) {
    assert(addr==HEADER_ADDR && len==32);memcpy(buf,flash_header,32);
  }
  return norflash_read(NULL,buf,len,addr);
}
int norflash_ioctl(struct device *dev,u32 cmd,u32 addr) {
  assert(dev==NULL);calls++;command=cmd;address=addr;return result;
}
int norflash_protect_suspend(void) { suspended++;return 0; }
int norflash_protect_resume(void) { resumed++;return 0; }
int main(void) {
  u8 data[32];
  u32 (*operations[])(u8 *,u32,u32)={dev_upgrade_read,
    dev_upgrade_origin_read,dev_upgrade_write};
  for (unsigned i=0;i<3;i++) {
    result=32;calls=0;
    assert(operations[i](data,0x37c000,32)==32);
    assert(calls==1 && buffer==data && address==0x37c000 && length==32);
    result=31;assert(operations[i](data,0x37c000,32)==0);
    result=-1;assert(operations[i](data,0x37c000,32)==0);
  }
  const u32 cmds[]={201,200,204};
  for (unsigned i=0;i<3;i++) {
    calls=0;assert(dev_upgrade_erase(i+1,0x4000)==1);
    assert(calls==1 && command==cmds[i] && address==0x4000);
  }
  calls=0;assert(dev_upgrade_erase(0,0)==0);
  assert(dev_upgrade_erase(4,0)==0 && calls==0);
  switch_upgrade_dev(1);assert(get_app_boot_base_addr()==0x4020);
  dev_upgrade_protect_suspend();dev_upgrade_protect_resume();
  assert(suspended==1 && resumed==1);
  emulate_flash=1;result=32;memset(flash_header,0xff,32);
  memset(data,0x5a,32);u8 copy[32];
  assert(h2_jieli_upgrade_header_copy(copy)==-1);
  assert(h2_jieli_upgrade_header_arm()==0);
  assert(h2_jieli_upgrade_header_arm()==-1);
  assert(dev_upgrade_origin_read(copy,HEADER_ADDR,32)==0);
  assert(dev_upgrade_write(data,HEADER_ADDR,32)==32);
  assert(physical_writes==0 && erased(flash_header));
  assert(h2_jieli_upgrade_header_copy(copy)==0 && memcmp(copy,data,32)==0);
  assert(dev_upgrade_origin_read(copy,HEADER_ADDR,32)==32);
  assert(dev_upgrade_write(data,HEADER_ADDR,32)==32 && physical_writes==0);
  data[0]^=1;
  assert(dev_upgrade_write(data,HEADER_ADDR,32)==0);
  assert(h2_jieli_upgrade_header_copy(copy)==-1 && erased(flash_header));
  assert(dev_upgrade_write(data,HEADER_ADDR,32)==0 && physical_writes==0);
  for(unsigned offset=0;offset<32;offset++) {
    header_gate=GATE_OFF;assert(h2_jieli_upgrade_header_arm()==0);
    assert(dev_upgrade_write(data,HEADER_ADDR+offset,1)==0);
    assert(dev_upgrade_write(data,HEADER_ADDR,32)==0 && physical_writes==0);
  }
  header_gate=GATE_OFF;assert(h2_jieli_upgrade_header_arm()==0);
  assert(dev_upgrade_write(data,HEADER_ADDR-1,32)==0);
  assert(h2_jieli_upgrade_header_publish(data)==-1); /* P1 cannot publish */
  base=H2_JIELI_BANK_2_SFC_BASE;
  assert(h2_jieli_upgrade_header_arm()==-1);
  assert(h2_jieli_upgrade_header_publish(data)==0);
  assert(physical_writes==1 && memcmp(data,flash_header,32)==0);
  assert(h2_jieli_upgrade_header_publish(data)==0 && physical_writes==1);
  data[1]^=1;assert(h2_jieli_upgrade_header_publish(data)==-1);
  assert(physical_writes==1); /* never erase/rewrite a conflicting header */
  memset(flash_header,0xff,32);result=31;
  assert(h2_jieli_upgrade_header_publish(data)==-1 && physical_writes==1);
  result=32;memset(data,0xff,32);
  assert(h2_jieli_upgrade_header_publish(data)==-1 && physical_writes==1);
  /* Fault injection: a driver reports success but only a prefix reached NOR.
   * Readback must reject every partial header. A simulated restart clears
   * volatile gating, not flash; a retry must not claim recovery or erase it.
   * This checks adapter behavior, not ROM selection after physical power loss.
   */
  memset(data,0x5a,32);
  for (unsigned prefix=1;prefix<32;prefix++) {
    memset(flash_header,0xff,32);programmed_bytes=prefix;
    int before=physical_writes;
    assert(h2_jieli_upgrade_header_publish(data)==-1);
    assert(physical_writes==before+1);
    assert(memcmp(flash_header,data,prefix)==0);
    for(unsigned i=prefix;i<32;i++) assert(flash_header[i]==0xff);
    header_gate=GATE_OFF;programmed_bytes=32;
    assert(h2_jieli_upgrade_header_publish(data)==-1);
    assert(physical_writes==before+1);
    base=H2_JIELI_BANK_1_SFC_BASE;
    assert(h2_jieli_upgrade_header_arm()==-1);
    base=H2_JIELI_BANK_2_SFC_BASE;
  }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "asm").mkdir()
            (root / "device").mkdir()
            (root / "app_config.h").write_text("")
            (root / "asm/sfc_norflash_api.h").write_text(declarations)
            (root / "device/ioctl_cmds.h").write_text(
                "#define IOCTL_ERASE_BLOCK 201\n#define IOCTL_ERASE_SECTOR 200\n"
                "#define IOCTL_ERASE_PAGE 204\n")
            (root / "adapter.c").write_text(SOURCE.read_text())
            (root / "test.c").write_text(program)
            command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-I", str(root),
                       "-I", str(SOURCE.parent.parent / "include"),
                       "-I", str(ROOT / "boards/jieli_ac791n_devkit/ac791n/include"),
                       str(root / "test.c"), "-o", str(root / "test")]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(root / "test")], check=True, timeout=10)
            rejected = subprocess.run(command + ["-DCONFIG_SDFILE_EXT_ENABLE=1"],
                                      capture_output=True, text=True, timeout=60)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("supports internal NOR only", rejected.stderr)
