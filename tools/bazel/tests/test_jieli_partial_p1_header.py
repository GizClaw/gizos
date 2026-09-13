"""Safety guards for the diagnostic P1 partial-header interruption."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class PartialP1Test(unittest.TestCase):
    def test_guards_and_recovery_does_not_pause_twice(self):
        source = (ROOT / "projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit/src/loader_partial_p1_header.c").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith('#include "'))
        stub = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <setjmp.h>
#include "jieli_native_image.h"
#define H2_JIELI_BANK_1_SFC_BASE 0x4020u
#define H2_JIELI_BANK_2_SFC_BASE 0x37c020u
int norflash_origin_read(uint8_t *,uint32_t,uint32_t);
int norflash_write(void *,void *,uint32_t,uint32_t);
int norflash_protect_suspend(void);
int norflash_protect_resume(void);
void wdt_close(void);
void os_time_dly(unsigned);
'''
        main = r'''
static uint8_t p1[32],p2[32];
static uint32_t base;
static int writes, closed, read_error;
static jmp_buf stopped;
uint32_t boot_info_get_sfc_base_addr(void) { return base; }
int decode_data_by_user_key(uint16_t k,uint8_t *b,uint16_t n,uint32_t a,uint8_t s) {
 (void)k;(void)b;(void)n;(void)a;(void)s;return 0;
}
int norflash_origin_read(uint8_t *out,uint32_t addr,uint32_t len) {
 if(read_error)return -1;
 assert(len==32); assert(addr==0x4000 || addr==0x37c000);
 memcpy(out,addr==0x4000?p1:p2,32);return 32;
}
int norflash_write(void *u,void *data,uint32_t len,uint32_t addr) {
 assert(!u && len==16 && addr==0x4000);memcpy(p1,data,16);writes++;return 16;
}
int norflash_protect_suspend(void){return 0;}
int norflash_protect_resume(void){return 0;}
void wdt_close(void){closed++;}
void os_time_dly(unsigned n){assert(n==100);longjmp(stopped,1);}
int main(void){
 uint8_t data[32]; memset(data,0x5a,32);
 base=H2_JIELI_BANK_1_SFC_BASE;
 h2_jieli_wl82_boot_probe(105); assert(!armed);
 base=H2_JIELI_BANK_2_SFC_BASE; read_error=1;
 h2_jieli_wl82_boot_probe(105); assert(!armed);read_error=0;
 h2_jieli_wl82_boot_probe(105); assert(armed);
 memset(p1,0xff,32);p2[0]=1;
 h2_jieli_upgrade_write_observer(data,0x4000,32);assert(!writes);
 p2[0]=0;
 h2_jieli_upgrade_write_observer(data,0x4000,15);assert(!writes);
 h2_jieli_upgrade_write_observer(data,0x37c000,32);assert(!writes);
 if(!setjmp(stopped)){h2_jieli_upgrade_write_observer(data,0x4000,32);assert(0);}
 assert(writes==1 && closed==1);assert(!memcmp(p1,data,16));
 assert(h2_jieli_loader_powercut_paused());
 for(unsigned i=16;i<32;i++)assert(p1[i]==0xff);
 for(unsigned i=0;i<32;i++)assert(p2[i]==0);
 h2_jieli_wl82_boot_probe(105);assert(!armed);
 h2_jieli_upgrade_write_observer(data,0x4000,32);assert(writes==1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(stub + source + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/include"),
                            str(path / "test.c"), "-o", str(path / "test")], check=True, timeout=60)
            subprocess.run([str(path / "test")], check=True, timeout=10)
