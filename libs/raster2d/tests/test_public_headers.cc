#include "h2_raster2d.h"
#include <cassert>

int main() {
  uint16_t a = 0xf800, b = 0x001f, palette = 0, pixel = 0;
  h2_raster2d_surface_t surface = {&pixel, 1, 1, 1, 1};
  h2_raster2d_rect_t rect = {0, 0, 1, 1, 0};
  assert(h2_raster2d_palette_blend(&a, &b, 1, 128, &palette, 1) == H2_PAL_OK);
  assert(h2_raster2d_draw_rects(&surface, &rect, 1, &palette, 1, nullptr) ==
         H2_PAL_OK);
  assert(pixel == 0x8010);
  return 0;
}
