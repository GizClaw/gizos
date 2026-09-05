"""Exercise the actual display preflight with overflow-sized rectangles."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class DisplayBoundsTest(unittest.TestCase):
    def test_rectangles_are_rejected_before_hardware_access(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/"
                  "h2_jieli_ac791n_devkit_input.c").read_text()
        begin = source.index("static int display_draw_bitmap(")
        end = source.index("  /* Official JieLi LCD fills", begin)
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
enum { H2_LCD_WIDTH=480, H2_LCD_HEIGHT=320,
 H2_DISPLAY_ERR_INVALID_STATE=-2, H2_DISPLAY_ERR_INVALID_ARG=-1,
 H2_DISPLAY_PIXEL_RGB565=1 };
typedef int h2_display_pixel_format_t;
typedef struct { int x,y,width,height; } h2_display_rect_t;
typedef struct { int open; } h2_display_state_t;
'''
        main = r'''
int main(void) {
 h2_display_state_t state={1}; uint16_t pixel=0;
 const h2_display_rect_t invalid[]={
  {INT_MAX,0,1,1},{0,INT_MAX,1,1},
  {1,0,INT_MAX,1},{0,1,1,INT_MAX},
  {INT_MAX,INT_MAX,INT_MAX,INT_MAX},
  {-1,0,1,1},{0,-1,1,1},{0,0,0,1},{0,0,1,0},
  {480,0,1,1},{0,320,1,1},{479,319,2,1},{479,319,1,2}};
 for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
  assert(display_draw_bitmap(&state,&invalid[i],&pixel,SIZE_MAX,1)==-1);
 h2_display_rect_t full={0,0,480,320}, corner={479,319,1,1};
 assert(display_draw_bitmap(&state,&full,&pixel,960,1)==0);
 assert(display_draw_bitmap(&state,&corner,&pixel,2,1)==0);
 assert(display_draw_bitmap(&state,&full,&pixel,959,1)==-1);
 assert(display_draw_bitmap(&state,NULL,&pixel,960,1)==-1);
 assert(display_draw_bitmap(&state,&full,NULL,960,1)==-1);
 assert(display_draw_bitmap(&state,&full,&pixel,960,0)==-1);
 state.open=0;
 assert(display_draw_bitmap(&state,&full,&pixel,960,1)==-2);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-display-bounds-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + source[begin:end] + "return 0;\n}\n" + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", "-fno-sanitize-recover=all",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
