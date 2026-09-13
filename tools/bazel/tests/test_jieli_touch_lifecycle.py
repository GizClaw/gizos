"""Fault-inject the real touch open/close functions without device access."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class TouchLifecycleTest(unittest.TestCase):
    def test_configuration_failures_unwind_and_allow_retry(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_input.c').read_text()
        opening = source[source.index('static h2_pal_result_t touch_open('):source.index('static h2_pal_result_t touch_get_info(')]
        closing = source[source.index('static h2_pal_result_t touch_close('):source.index('const h2_pal_touch_api_t *')]
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include "h2/pal/hal/h2_pal_touch.h"
typedef struct { void *iic; int open, down; uint16_t x,y; } h2_touch_state_t;
enum { H2_TOUCH_INT_PIN=4, H2_FT6236_CHIP_ID=0xa3 };
static int fail_write, writes, opens, closes;
static void gpio_set_direction(int p,int v) {(void)p;(void)v;}
static void gpio_set_pull_up(int p,int v) {(void)p;(void)v;}
static void gpio_set_pull_down(int p,int v) {(void)p;(void)v;}
static void delay_ms(uint32_t ms) {(void)ms;}
static void *dev_open(const char *n,void *a) {(void)n;(void)a; ++opens; return &opens;}
static int dev_close(void *p) {assert(p==&opens); ++closes; return 0;}
static int touch_read_register(uint8_t r,uint8_t *v) {assert(r==H2_FT6236_CHIP_ID); *v=0x64; return 0;}
static int touch_write_register(uint8_t r,uint8_t v) {(void)r;(void)v; return ++writes==fail_write ? -1:0;}
'''
        main = r'''
int main(void) {
 for (int fault=1; fault<=3; ++fault) {
  h2_touch_state_t state={0};
  writes=opens=closes=0; fail_write=fault;
  assert(touch_open(&state)==H2_PAL_ERR_IO);
  assert(!state.open && state.iic==NULL && opens==1 && closes==1);
  assert(writes==fault);
  assert(touch_close(&state)==H2_PAL_OK && closes==1);
  fail_write=0; writes=0;
  assert(touch_open(&state)==H2_PAL_OK && writes==3 && opens==2);
  assert(touch_open(&state)==H2_PAL_OK && opens==2);
  assert(touch_close(&state)==H2_PAL_OK && closes==2);
  assert(touch_close(&state)==H2_PAL_OK && closes==2);
 }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(stub + opening + closing + main)
            binary = Path(directory) / 'test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'libs/pal/include'), str(test), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
