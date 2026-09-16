"""Validate the persisted candidate identity and decoded native boot header."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class PendingBootTest(unittest.TestCase):
    def test_record_and_header_must_match_candidate(self):
        program = r'''
#include <assert.h>
#include "jieli_pending_boot.h"
static void crc(uint8_t *p) {
  uint16_t c=h2_jieli_native_header_crc(p);p[0]=(uint8_t)c;p[1]=(uint8_t)(c>>8);
}
int main(void) {
  h2_jieli_pending_boot_t r={0};
  r.magic=H2_JIELI_PENDING_BOOT_MAGIC;r.code_length=1024;r.code_crc=0x1234;
  memset(r.image_checksum,'a',64);
  char sha[65];memcpy(sha,r.image_checksum,65);
  assert(h2_jieli_pending_boot_matches(&r,sizeof(r),sha));
  uint8_t wire[H2_JIELI_PENDING_BOOT_WIRE_SIZE];
  h2_jieli_pending_boot_encode(&r,wire);
  assert(sizeof(wire)==112 && wire[0]=='H' && wire[1]=='P');
  assert(wire[4]==0 && wire[5]==4 && wire[8]==0x34 && wire[9]==0x12);
  h2_jieli_pending_boot_t decoded;
  assert(!h2_jieli_pending_boot_decode(NULL,wire,sizeof(wire)));
  assert(!h2_jieli_pending_boot_decode(&decoded,NULL,sizeof(wire)));
  assert(h2_jieli_pending_boot_decode(&decoded,wire,sizeof(wire)));
  assert(h2_jieli_pending_boot_matches(&decoded,sizeof(decoded),sha));
  assert(decoded.code_length==r.code_length && decoded.code_crc==r.code_crc);
  for(unsigned i=0;i<32;++i) r.header[i]=(uint8_t)(i+128u);
  h2_jieli_pending_boot_encode(&r,wire);
  for(unsigned i=0;i<32;++i) assert(wire[77u+i]==(uint8_t)(i+128u));
  assert(h2_jieli_pending_boot_decode(&decoded,wire,sizeof(wire)));
  assert(memcmp(decoded.header,r.header,32)==0);
  uint8_t reencoded[112];h2_jieli_pending_boot_encode(&decoded,reencoded);
  assert(memcmp(wire,reencoded,sizeof(wire))==0);
  for(size_t n=0;n<sizeof(wire);++n)
    assert(!h2_jieli_pending_boot_decode(&decoded,wire,n));
  assert(!h2_jieli_pending_boot_decode(&decoded,wire,sizeof(wire)+1));
  for(unsigned n=109;n<112;++n) {
    wire[n]=1;assert(!h2_jieli_pending_boot_decode(&decoded,wire,sizeof(wire)));
    wire[n]=0;
  }
  assert(!h2_jieli_pending_boot_matches(NULL,sizeof(r),sha));
  assert(!h2_jieli_pending_boot_matches(&r,sizeof(r),NULL));
  for(size_t i=0;i<sizeof(r);++i)
    assert(!h2_jieli_pending_boot_matches(&r,i,sha));
  assert(!h2_jieli_pending_boot_matches(&r,sizeof(r)+1,sha));
  h2_jieli_pending_boot_t bad=r;bad.magic^=1;
  assert(!h2_jieli_pending_boot_matches(&bad,sizeof(bad),sha));
  bad=r;bad.reserved=1;assert(!h2_jieli_pending_boot_matches(&bad,sizeof(bad),sha));
  bad=r;bad.image_checksum[64]='a';
  assert(!h2_jieli_pending_boot_matches(&bad,sizeof(bad),sha));
  bad=r;bad.image_checksum[0]='z';
  assert(!h2_jieli_pending_boot_matches(&bad,sizeof(bad),sha));
  sha[0]='b';assert(!h2_jieli_pending_boot_matches(&r,sizeof(r),sha));sha[0]='a';
  bad=r;bad.code_length=0;assert(!h2_jieli_pending_boot_matches(&bad,sizeof(bad),sha));
  bad.code_length=H2_JIELI_IMAGE_MAX_SIZE+1;
  assert(!h2_jieli_pending_boot_matches(&bad,sizeof(bad),sha));
  uint8_t header[32]={0};header[4]=0x34;header[5]=0x12;header[9]=4;crc(header);
  assert(h2_jieli_pending_boot_header_valid(&r,header));
  for(unsigned i=0;i<32;i++)for(unsigned bit=0;bit<8;bit++) {
    uint8_t corrupt[32];memcpy(corrupt,header,32);corrupt[i]^=1u<<bit;
    assert(!h2_jieli_pending_boot_header_valid(&r,corrupt));
  }
  header[4]^=1;crc(header);assert(!h2_jieli_pending_boot_header_valid(&r,header));
  header[4]^=1;header[9]^=1;crc(header);
  assert(!h2_jieli_pending_boot_header_valid(&r,header));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.c"
            source.write_text(program)
            binary = Path(directory) / "test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Werror",
                "-I", str(ROOT / "boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/include"),
                "-I", str(ROOT / "boards/jieli_ac791n_devkit/ac791n/include"),
                str(source), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)
