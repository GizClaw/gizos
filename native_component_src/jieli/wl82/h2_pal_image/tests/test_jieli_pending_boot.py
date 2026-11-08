"""Preserve pending112 bytes and separately verify native boot evidence."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[5]


class PendingBootTest(unittest.TestCase):
    def test_record_and_header_must_match_candidate(self):
        provider = (ROOT / "native_component_src/jieli/wl82/h2_pal_image/src/h2_jieli_wl82_platform_image.c").read_text()
        native_check = provider[provider.index("static int native_header_valid("):provider.index("static int native_burn(")]
        program = r'''
#include <assert.h>
#include "jieli_pending_boot.h"
#include "jieli_native_image.h"
#define BOOT_EVIDENCE_V1 1u
#define BOOT_EVIDENCE_SIZE 40u
#define H2_JIELI_UPGRADE_HEADER_SIZE 32u
static struct { struct { uint32_t image_capacity; } config; } state = {{0x37d000u}};
static uint32_t decode_data_by_user_key(uint16_t key,uint8_t *p,uint16_t n,uint32_t a,uint8_t b) {
  assert(key==0xffff && n==32 && a==0 && b==32);(void)p;return 0;
}
/* NATIVE_CHECK */
static void crc(uint8_t *p) {
  uint16_t c=h2_jieli_native_header_crc(p);p[0]=(uint8_t)c;p[1]=(uint8_t)(c>>8);
}
int main(void) {
  char sha[65];memset(sha,'a',64);sha[64]=0;
  h2_pal_power_native_evidence_t evidence={.format=1,.size=40};
  evidence.bytes[1]=4; evidence.bytes[4]=0x34; evidence.bytes[5]=0x12;
  uint8_t *header=evidence.bytes+8;
  header[4]=0x34;header[5]=0x12;header[9]=4;crc(header);
  assert(native_header_valid(&evidence));
  uint8_t wire[112], golden[112]={0x48,0x50,0x42,0x31,0,4,0,0,0x34,0x12,0,0};
  memcpy(golden+12,sha,65);memcpy(golden+77,header,32);
  assert(h2_jieli_pending_boot_encode(&evidence,sha,wire));
  assert(memcmp(wire,golden,112)==0);
  h2_pal_power_native_evidence_t decoded;
  assert(h2_jieli_pending_boot_decode(wire,112,sha,&decoded));
  assert(memcmp(&evidence,&decoded,sizeof(evidence))==0);
  assert(native_header_valid(&decoded));
  for(size_t n=0;n<112;++n) assert(!h2_jieli_pending_boot_decode(wire,n,sha,&decoded));
  assert(!h2_jieli_pending_boot_decode(wire,113,sha,&decoded));
  assert(!h2_jieli_pending_boot_decode(NULL,112,sha,&decoded));
  assert(!h2_jieli_pending_boot_decode(wire,112,NULL,&decoded));
  assert(!h2_jieli_pending_boot_decode(wire,112,sha,NULL));
  for(unsigned i=0;i<112;++i) {
    if(i>=77 && i<109) continue; /* Header facts belong only to the provider. */
    uint8_t bad[112];memcpy(bad,wire,112);bad[i]^=1;
    if(i>=4 && i<10) continue; /* Code facts are also checked natively. */
    assert(!h2_jieli_pending_boot_decode(bad,112,sha,&decoded));
  }
  for(unsigned i=0;i<32;++i)for(unsigned bit=0;bit<8;++bit) {
    h2_pal_power_native_evidence_t bad=evidence;bad.bytes[8+i]^=1u<<bit;
    assert(!native_header_valid(&bad));
  }
  h2_pal_power_native_evidence_t bad=evidence;bad.bytes[4]^=1;
  assert(!native_header_valid(&bad));
  bad=evidence;bad.bytes[1]^=1;assert(!native_header_valid(&bad));
  bad=evidence;memset(bad.bytes,0,4);assert(!native_header_valid(&bad));
  bad=evidence;memset(bad.bytes,0xff,4);assert(!native_header_valid(&bad));
  bad=evidence;bad.format=2;assert(!h2_jieli_pending_boot_encode(&bad,sha,wire));
  bad=evidence;bad.size=39;assert(!h2_jieli_pending_boot_encode(&bad,sha,wire));
  bad=evidence;bad.bytes[6]=1;assert(!h2_jieli_pending_boot_encode(&bad,sha,wire));
  sha[0]='A';assert(!h2_jieli_pending_boot_encode(&evidence,sha,wire));
  sha[0]='a';sha[64]='x';assert(!h2_jieli_pending_boot_encode(&evidence,sha,wire));
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.c"
            source.write_text(program.replace("/* NATIVE_CHECK */", native_check))
            binary = Path(directory) / "test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "libs/h2loader/device/include"),
                "-I", str(ROOT / "native_component_src/jieli/wl82/h2_pal_image/include"),
                "-I", str(ROOT / "pal/include"),
                str(source), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
