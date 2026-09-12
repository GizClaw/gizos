"""Execute the shared retained boot-request validation used by early main."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class WarmRequestTest(unittest.TestCase):
    def test_invalid_torn_and_replayed_requests_fail_closed(self):
        program = r'''
#include <assert.h>
#include <stddef.h>
#include "h2_jieli_warm_request.h"
int main(void) {
  _Static_assert(sizeof(h2_jieli_warm_request_t)==24, "retained ABI");
  _Static_assert(offsetof(h2_jieli_warm_request_t,inflight)==20, "retained ABI");
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
                            str(ROOT / "boards/jieli_ac791n_devkit/ac791n/include"),
                            str(source), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)
        early = (ROOT / "boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/"
                 "src/jieli_warm_boot.c").read_text()
        self.assertIn("h2_jieli_warm_request_take(REQUEST)", early)
        self.assertIn("*(volatile uint32_t *)(handoff + 8) = "
                      "*(const volatile uint32_t *)(boot_info + 20);", early)
