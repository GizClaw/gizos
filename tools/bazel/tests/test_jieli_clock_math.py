"""Check oscillator conversion and pending-interrupt epoch arithmetic."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class ClockMathTest(unittest.TestCase):
    def test_ticks_and_epoch_conversion(self):
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include "h2_jieli_ac791n_clock_math.h"
int main(void) {
 const uint32_t clocks[] = {12000000, 24000000, 26000000, 40000000, 48000000};
 for (unsigned i=0; i<sizeof(clocks)/sizeof(clocks[0]); ++i) {
  uint64_t last=0;
  for (uint64_t tick=0; tick<200000; ++tick) {
   uint64_t us=h2_jieli_clock_ticks_to_us(tick, clocks[i]);
   assert(us == (uint64_t)(((__uint128_t)tick*8192*1000000)/clocks[i]));
   assert(us >= last); last=us;
  }
  /* A multiply-first uint64 implementation overflows at this duration. */
  const uint64_t long_ticks=UINT64_C(1000000000000);
  assert(h2_jieli_clock_ticks_to_us(long_ticks,clocks[i]) ==
         (uint64_t)(((__uint128_t)long_ticks*8192*1000000)/clocks[i]));
 }
 const uint64_t base=UINT64_C(0x100000000);
 const uint64_t before=h2_jieli_clock_snapshot_ticks(base,65534,0);
 const uint64_t pending=h2_jieli_clock_snapshot_ticks(base,0,1);
 const uint64_t serviced=h2_jieli_clock_snapshot_ticks(base+65535,0,0);
 assert(before+1 == pending && pending == serviced);
 assert(h2_jieli_clock_ticks_to_us(1,24000000) == 341);
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-jieli-clock-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(fixture)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", "-I",
                            str(ROOT / "boards/jieli_ac791n_devkit/ac791n/include"),
                            str(test), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
