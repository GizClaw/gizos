"""Exercise actual advertising encoder length validation before copies."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class BleAdvBoundsTest(unittest.TestCase):
    def test_service_payload_overflow(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_ble.c').read_text()
        funcs = source[source.index('static int h2_adv_append('):source.index('static uint8_t h2_adv_phy(')]
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_ble.h"
enum { H2_JIELI_ADV_DATA_MAX=251 };
static void *checked_copy(void *dst,const void *src,size_t len) {
 assert(len<=H2_JIELI_ADV_DATA_MAX);
 return memcpy(dst,src,len);
}
#undef memcpy
#define memcpy checked_copy
'''
        main = r'''
int main(void) {
 uint8_t bytes[251]={0}, output[251], used;
 h2_pal_ble_adv_data_t data={0};
 const size_t lengths[]={SIZE_MAX,SIZE_MAX-1u,SIZE_MAX-15u,250u,251u};
 for (unsigned uuid=2;uuid<=16;uuid+=14) {
  data.service_data_uuid=(h2_pal_ble_uuid_t){bytes,uuid};
  for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i) {
   data.service_data=(h2_pal_ble_bytes_t){bytes,lengths[i]}; used=99;
   assert(h2_encode_adv(&data,output,sizeof(output),&used)==H2_PAL_ERR_INVALID_ARG);
   assert(used==99);
  }
  data.service_data=(h2_pal_ble_bytes_t){bytes,251u-uuid-5u};
  assert(h2_encode_adv(&data,output,sizeof(output),&used)==H2_PAL_OK && used==251);
  ++data.service_data.len;
  assert(h2_encode_adv(&data,output,sizeof(output),&used)==H2_PAL_ERR_NO_SPACE);
 }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(stub + funcs + main)
            binary = Path(directory) / 'test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'libs/pal/include'), str(test), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
