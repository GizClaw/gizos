"""Execute the shared retained boot-request validation used by early main."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[5]


class WarmRequestTest(unittest.TestCase):
    def test_real_early_handoff_keeps_cpu_address_and_retained_layout(self):
        native_root = ROOT / "native_component_src/jieli/wl82/h2_pal_image"
        early = (native_root / "src/jieli_warm_boot_early.c").read_text()
        common = (native_root / "src/jieli_warm_boot.c").read_text()
        def function(text, signature):
            begin = text.index(signature)
            end = text.index("{", begin) + 1
            depth = 1
            while depth:
                depth += (text[end] == "{") - (text[end] == "}")
                end += 1
            return text[begin:end]
        remap = function(early, "__attribute__((noinline)) static void remap_flash(")
        self.assertLess(remap.index("flush_all_cache();"), remap.index("JL_SFC->BASE_ADR = sfc_base"))
        self.assertIn("AT(.volatile_ram_code)", early)
        types = common[common.index("typedef struct warm_snapshot"):common.index("#define SNAPSHOT (")]
        code = function(common, "void h2_jieli_warm_boot_snapshot_previous(")
        code += function(common, "void h2_jieli_warm_boot_mark_handoff(")
        code += function(early, "void h2_jieli_wl82_warm_boot_early(")
        code = code.replace("(uint32_t)_HEAP_END", "(uint32_t)(uintptr_t)_HEAP_END")
        program = r'''
#include <assert.h>
#include <string.h>
#include "h2_jieli_warm_request.h"
#define RETAINED_LOG_CAPACITY 2048u
#define SNAPSHOT_MAGIC UINT32_C(0x50414e53)
/* TYPES */
static h2_jieli_warm_request_t request;
static warm_snapshot_t snapshot;
static uint32_t marker[4];
static uint8_t log_bytes[2060] __attribute__((aligned(4)));
static uint8_t boot_bytes[40] __attribute__((aligned(4)));
static uint8_t sdram[8],extra[4];
static uint8_t handoff[96] __attribute__((aligned(4)));
static uint8_t flash_head[32] __attribute__((aligned(4)));
static struct { uint32_t BASE_ADR; } sfc={H2_JIELI_BANK_1_SFC_BASE};
#define JL_SFC (&sfc)
#define REQUEST (&request)
#define SNAPSHOT (&snapshot)
#define RAM_MARKER_ADDR ((uintptr_t)marker)
#define RETAINED_LOG_ADDR ((uintptr_t)log_bytes)
#define BOOT_INFO_ADDR ((uintptr_t)boot_bytes)
#define SDRAM_INFO_ADDR ((uintptr_t)sdram)
#define EX_APP_INFO_ADDR ((uintptr_t)extra)
#define WARM_FLASH_HEAD_ADDR ((uintptr_t)flash_head)
#define WARM_HANDOFF_ADDR ((uintptr_t)handoff)
#define FLASH_WINDOW UINT32_C(0x02000000)
#define FLASH_WINDOW_SIZE UINT32_C(0x00800000)
#define _HEAP_END ((char*)UINT32_C(0x04100000))
static unsigned irq_disabled,flushes,jumps;
static uintptr_t flush_address[3];
static int flush_size[3];
const h2_jieli_wl82_boot_geometry_t* h2_jieli_wl82_boot_geometry(void) {
 static const h2_jieli_wl82_boot_geometry_t geometry={H2_JIELI_BANK_1_SFC_BASE,H2_JIELI_BANK_2_SFC_BASE,0x740000};
 return &geometry;
}
static void __local_irq_disable(void) { assert(!irq_disabled);irq_disabled=1; }
static void flushinv_dcache(void *address,int size) {
 assert(irq_disabled && flushes<3);flush_address[flushes]=(uintptr_t)address;flush_size[flushes++]=size;
}
static void warm_jump(uint32_t base,const void *data) {
 assert(base==H2_JIELI_BANK_2_SFC_BASE && data==handoff && irq_disabled && flushes==3);
 assert(request.magic==0 && request.inflight==H2_JIELI_WARM_INFLIGHT);++jumps;
}
/* CODE */
static void put32(uint8_t *p,uint32_t value) { memcpy(p,&value,4); }
static uint32_t get32(const uint8_t *p) { uint32_t value;memcpy(&value,p,4);return value; }
int main(void) {
 /* No request, or a torn request, must not touch caches or enter another image. */
 h2_jieli_wl82_warm_boot_early();assert(!jumps && !flushes);
 request.magic=H2_JIELI_WARM_MAGIC;request.sfc_base=H2_JIELI_BANK_2_SFC_BASE;
 request.check=0;h2_jieli_wl82_warm_boot_early();assert(!jumps && !flushes && request.magic==0);
 marker[0]=1;marker[1]=2;marker[2]=105;marker[3]=7;
 put32(log_bytes,0x474f4c49);put32(log_bytes+4,1);put32(log_bytes+8,1);log_bytes[12]='x';
 request=(h2_jieli_warm_request_t){.magic=H2_JIELI_WARM_MAGIC,.sfc_base=H2_JIELI_BANK_2_SFC_BASE,
  .check=h2_jieli_warm_request_check(H2_JIELI_BANK_2_SFC_BASE),.count=2,.inflight=H2_JIELI_WARM_INFLIGHT};
 boot_bytes[0]=16;put32(boot_bytes+4,123);put32(boot_bytes+20,0x02012340);put32(boot_bytes+24,0x800000);
 boot_bytes[28]=0x34;boot_bytes[29]=0x12;boot_bytes[30]=0x78;boot_bytes[31]=0x56;
 for(unsigned i=0;i<8;++i) { boot_bytes[32+i]=(uint8_t)(i+1);sdram[i]=(uint8_t)(i+10); }
 for(unsigned i=0;i<4;++i)extra[i]=(uint8_t)(i+20);
 h2_jieli_wl82_warm_boot_early();
 assert(jumps==1 && request.count==3 && request.last_base==H2_JIELI_BANK_2_SFC_BASE);
 assert(snapshot.magic==SNAPSHOT_MAGIC && snapshot.marker[2]==105 && snapshot.log[0]=='x');
 assert(marker[2]==UINT32_C(0xabcd0001));
 assert(get32(handoff)==(uint32_t)(uintptr_t)flash_head && get32(handoff+4)==H2_JIELI_BANK_2_SFC_BASE);
 assert(get32(handoff+8)==0x02012340 && get32(handoff+8)!=123); /* CPU address, never codeLength. */
 assert(get32(flash_head+8)==0x800000 && flash_head[13]==16);
 assert(memcmp(handoff+12,boot_bytes+28,4)==0 && memcmp(handoff+16,boot_bytes+32,8)==0);
 assert(memcmp(handoff+80,sdram,8)==0 && memcmp(handoff+88,extra,4)==0);
 assert(flush_address[0]==0x01c00000 && flush_size[0]==0x80000);
 assert(flush_address[1]==0x04000000 && flush_size[1]==0x100020);
 assert(flush_address[2]==FLASH_WINDOW && flush_size[2]==FLASH_WINDOW_SIZE);
 /* Arrival in the requested mapping preserves trial evidence and cannot
  * consume/jump again. A later ROM return snapshots the old image once. */
 sfc.BASE_ADR=H2_JIELI_BANK_2_SFC_BASE;h2_jieli_wl82_warm_boot_early();assert(jumps==1 && flushes==3);
 assert(h2_jieli_warm_request_take_origin(&request,sfc.BASE_ADR,
  H2_JIELI_BANK_1_SFC_BASE,H2_JIELI_BANK_2_SFC_BASE)==H2_JIELI_BANK_1_SFC_BASE);
 assert(h2_jieli_warm_request_take_origin(&request,sfc.BASE_ADR,
  H2_JIELI_BANK_1_SFC_BASE,H2_JIELI_BANK_2_SFC_BASE)==0);
 sfc.BASE_ADR=H2_JIELI_BANK_1_SFC_BASE;h2_jieli_wl82_warm_boot_early();
 assert(jumps==1 && flushes==3 && request.inflight==0 && request.count==3);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            # Retained storage must not shadow the provider's local pointer.
            source.write_text(program.replace("handoff", "handoff_memory").replace("/* TYPES */", types).replace("/* CODE */", code))
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "boards/jieli/jieli_ac791n_devkit/ac791n/include"),
                            "-I", str(native_root / "include"), str(source), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_invalid_torn_and_replayed_requests_fail_closed(self):
        program = r'''
#include <assert.h>
#include <stddef.h>
#include "h2_jieli_warm_request.h"
int main(void) {
  _Static_assert(offsetof(h2_jieli_warm_request_t,magic)==0, "retained ABI");
  _Static_assert(offsetof(h2_jieli_warm_request_t,sfc_base)==4, "retained ABI");
  _Static_assert(offsetof(h2_jieli_warm_request_t,check)==8, "retained ABI");
  _Static_assert(offsetof(h2_jieli_warm_request_t,count)==12, "retained ABI");
  _Static_assert(offsetof(h2_jieli_warm_request_t,last_base)==16, "retained ABI");
  _Static_assert(offsetof(h2_jieli_warm_request_t,inflight)==20, "retained ABI");
  _Static_assert(offsetof(h2_jieli_warm_request_t,origin_base)==24, "origin receipt ABI");
  _Static_assert(offsetof(h2_jieli_warm_request_t,origin_check)==28, "origin receipt ABI");
  _Static_assert(offsetof(h2_jieli_warm_request_t,origin_magic)==32, "origin receipt ABI");
  _Static_assert(sizeof(h2_jieli_warm_request_t)==36, "origin receipt ABI");
  _Static_assert(sizeof(h2_jieli_warm_request_t)<=0x40, "must not overlap handoff");
  const uint32_t banks[]={H2_JIELI_BANK_1_SFC_BASE,H2_JIELI_BANK_2_SFC_BASE};
  h2_jieli_warm_request_t arriving={.sfc_base=H2_JIELI_BANK_2_SFC_BASE,
    .last_base=H2_JIELI_BANK_2_SFC_BASE,.inflight=H2_JIELI_WARM_INFLIGHT,
    .check=h2_jieli_warm_request_check(H2_JIELI_BANK_2_SFC_BASE)};
  assert(h2_jieli_warm_request_is_active_image(&arriving,banks[1]));
  assert(arriving.inflight==H2_JIELI_WARM_INFLIGHT);
  assert(!h2_jieli_warm_request_is_active_image(&arriving,banks[0]));
  for (unsigned i=0;i<2;++i) {
    assert(h2_jieli_warm_request_running_base(&arriving,banks[i],0,banks[i])==banks[i]);
    assert(h2_jieli_warm_request_running_base(&arriving,banks[i],0,banks[1-i])==0);
    assert(h2_jieli_warm_request_running_base(&arriving,banks[i],0,0)==0);
  }
  assert(h2_jieli_warm_request_running_base(&arriving,banks[1],-1,0)==banks[1]);
  assert(h2_jieli_warm_request_running_base(&arriving,banks[0],-1,0)==0);
  assert(h2_jieli_warm_request_running_base(&arriving,0x37c000,0,0x37c000)==0);
  for (unsigned bit=0;bit<32;++bit) {
    h2_jieli_warm_request_t corrupt=arriving;
    corrupt.check^=UINT32_C(1)<<bit;
    assert(!h2_jieli_warm_request_is_active_image(&corrupt,banks[1]));
    assert(h2_jieli_warm_request_running_base(&corrupt,banks[1],-1,0)==0);
  }
  arriving.magic=H2_JIELI_WARM_MAGIC;
  assert(!h2_jieli_warm_request_is_active_image(&arriving,banks[1]));
  arriving.magic=0;arriving.inflight=0;
  assert(!h2_jieli_warm_request_is_active_image(&arriving,banks[1]));
  for (unsigned i=0;i<2;++i) {
    h2_jieli_warm_request_t original={.magic=H2_JIELI_WARM_MAGIC,
      .sfc_base=banks[i],.check=h2_jieli_warm_request_check(banks[i])};
    h2_jieli_warm_request_t request=original;
    assert(h2_jieli_warm_request_take(&request)==banks[i]);
    assert(request.magic==0 && h2_jieli_warm_request_take(&request)==0);
    for (unsigned bit=0;bit<32;++bit) {
      request=original;request.magic^=UINT32_C(1)<<bit;
      assert(h2_jieli_warm_request_take(&request)==0 && request.magic==0);
      request=original;request.sfc_base^=UINT32_C(1)<<bit;
      assert(h2_jieli_warm_request_take(&request)==0 && request.magic==0);
      request=original;request.check^=UINT32_C(1)<<bit;
      assert(h2_jieli_warm_request_take(&request)==0 && request.magic==0);
    }
    request=original;request.magic=0; /* reset before final publication */
    assert(h2_jieli_warm_request_take(&request)==0);
  }
  const uint32_t bad[]={0,1,0x2000120,0x37c000,0xffffffff};
  for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
    h2_jieli_warm_request_t request={.magic=H2_JIELI_WARM_MAGIC,
      .sfc_base=bad[i],.check=h2_jieli_warm_request_check(bad[i])};
    assert(h2_jieli_warm_request_take(&request)==0 && request.magic==0);
  }
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.c"
            source.write_text(program)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I",
                            str(ROOT / "boards/jieli/jieli_ac791n_devkit/ac791n/include"),
                "-I", str(ROOT / "native_component_src/jieli/wl82/h2_pal_image/include"),
                            str(source), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
