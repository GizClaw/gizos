"""Exercise the board's asynchronous SD readiness gate with a fake SDK clock."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_sd_fs.c"


class SdReadyTest(unittest.TestCase):
    def test_immediate_delayed_missing_and_wrapping_clock(self):
        source = SOURCE.read_text()
        start = source.index("static int wait_sd_online(void) {")
        end = source.index("\nh2_pal_result_t h2_jieli_ac791n_devkit_sd_fs_init", start)
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define H2_JIELI_SD_READY_TIMEOUT_MS 5000u
#define H2_PAL_OK 0
#define H2_PAL_ERR_UNAVAILABLE -2
static uint32_t clock_ms, elapsed, ready_after, yields;
static uint32_t timer_get_ms(void) { return clock_ms; }
static int dev_online(const char *name) {
  assert(strcmp(name, "sd0") == 0); return elapsed >= ready_after;
}
static void os_time_dly(unsigned ticks) {
  assert(ticks == 5u); ++yields; elapsed += 50; clock_ms += 50;
}
static void reset(uint32_t clock, uint32_t ready) {
  clock_ms = clock; ready_after = ready; elapsed = yields = 0;
}
'''
        main = r'''
int main(void) {
  reset(200, 0);
  assert(wait_sd_online() == 0 && yields == 0);
  reset(200, 150);
  assert(wait_sd_online() == 0 && yields == 3 && elapsed == 150);
  reset(0, UINT32_MAX);
  assert(wait_sd_online() == -2 && elapsed == 5000 && yields == 100);
  reset(UINT32_MAX - 100, 200);
  assert(wait_sd_online() == 0 && elapsed == 200);
  reset(UINT32_MAX - 100, UINT32_MAX);
  assert(wait_sd_online() == -2 && elapsed == 5000);
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-sd-ready-test-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + source[start:end] + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", str(test), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
