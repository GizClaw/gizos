#include "tools/lvgl/ttf_subset/fixture_font.h"

#include <assert.h>

int main(void) {
  assert(fixture_font_size > 12u);
  assert(fixture_font_data[0] == 0x00u);
  assert(fixture_font_data[1] == 0x01u);
  assert(fixture_font_data[2] == 0x00u);
  assert(fixture_font_data[3] == 0x00u);
  return 0;
}
