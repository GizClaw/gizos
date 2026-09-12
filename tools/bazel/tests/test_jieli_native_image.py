"""Validate the native descriptor against an SDK-generated UFW header."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class NativeImageTest(unittest.TestCase):
    def test_native_code_descriptor_and_corruption(self):
        # Directory from the verified v14 Loader package, SDK eb04f1966c.
        header = bytes.fromhex(
            "231d25a8200000005d210e0003ff01007570646174655f6461746100ffffffff"
            "59e80d68400000001d210e0002ff00006170705f636f726500ffffffffffffff")
        program = '#include <assert.h>\n#include "jieli_native_image.h"\n'
        program += "static const unsigned char header[]={" + ",".join(map(str, header)) + "};\n"
        program += r'''
int main(void) {
  h2_jieli_native_image_t info={0};
  assert(h2_jieli_native_image_parse(header,sizeof(header),926045,&info)==0);
  assert(info.code_offset==64 && info.code_length==925981 && info.code_crc==0x680d);
  for (unsigned size=0;size<sizeof(header);++size)
    assert(h2_jieli_native_image_parse(header,size,926045,&info)!=0);
  assert(h2_jieli_native_image_parse(header,sizeof(header),926044,&info)!=0);
  unsigned char copy[sizeof(header)];
  for (unsigned byte=0;byte<sizeof(header);++byte) {
    for (unsigned bit=0;bit<8;++bit) {
      memcpy(copy,header,sizeof(header));copy[byte]^=1u<<bit;
      assert(h2_jieli_native_image_parse(copy,sizeof(copy),926045,&info)!=0);
    }
  }
  memcpy(copy,header,sizeof(header));
  memset(copy+40,0xff,4); /* length overflow, with a valid directory CRC */
  uint16_t crc=h2_jieli_native_header_crc(copy+32);
  copy[32]=(uint8_t)crc;copy[33]=(uint8_t)(crc>>8);
  assert(h2_jieli_native_image_parse(copy,sizeof(copy),926045,&info)!=0);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.c"
            source.write_text(program)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I",
                            str(ROOT / "boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/include"),
                            str(source), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)
