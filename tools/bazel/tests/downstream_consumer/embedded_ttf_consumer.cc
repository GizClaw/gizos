#include "embedded_menu_font.h"

#include <cassert>

int main() {
  assert(embedded_menu_font_size > 12u);
  assert(embedded_menu_font_data[0] == 0x00u);
  assert(embedded_menu_font_data[1] == 0x01u);
  assert(embedded_menu_font_data[2] == 0x00u);
  assert(embedded_menu_font_data[3] == 0x00u);
  return 0;
}
