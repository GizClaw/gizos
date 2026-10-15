#include "h2_example_raster2d.h"
#include "h2_raster2d.h"

h2_pal_result_t h2_example_raster2d_run(h2_runtime_t *runtime) {
  if (runtime == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t result = h2_pal_display_open(runtime->display);
  if (result != H2_PAL_OK)
    return result;
  h2_display_info_t info;
  uint16_t *pixels = NULL;
  result = h2_pal_display_get_info(runtime->display, &info);
  if (result != H2_PAL_OK)
    goto close;
  if (info.width != 240 || info.height != 240) {
    result = H2_PAL_ERR_UNSUPPORTED;
    goto close;
  }
  pixels = h2_pal_mem_alloc(runtime->mem, 240u * 240u * sizeof(uint16_t));
  if (pixels == NULL) {
    result = H2_PAL_ERR_NO_MEMORY;
    goto close;
  }
  for (size_t i = 0; i < 240u * 240u; ++i)
    pixels[i] = 0;
  const h2_raster2d_surface_t surface = {pixels, 240u * 240u, 240, 240, 240};
  const uint16_t a[] = {0xf800, 0xffff}, b[] = {0x001f, 0x07e0};
  const h2_raster2d_rect_t rects[] = {{-10, -10, 260, 260, 0},
                                      {20, 80, 200, 80, 1}};
  for (unsigned column = 0; column < 3; ++column) {
    uint16_t palette[2];
    h2_raster2d_clip_t clip = {column * 80u, 0, (column + 1u) * 80u, 240};
    result = h2_raster2d_palette_blend(a, b, 2, column * 128u, palette, 2);
    if (result != H2_PAL_OK)
      goto close;
    result = h2_raster2d_draw_rects(&surface, rects, 2, palette, 2, &clip);
    if (result != H2_PAL_OK)
      goto close;
  }
  const h2_display_rect_t screen = {0, 0, 240, 240};
  result = h2_pal_display_draw_bitmap(runtime->display, &screen, pixels,
                                      240u * sizeof(uint16_t),
                                      H2_DISPLAY_PIXEL_RGB565);
  if (result == H2_PAL_OK)
    result = h2_pal_display_present(runtime->display);
close:
  h2_pal_mem_free(runtime->mem, pixels);
  h2_pal_result_t closed = h2_pal_display_close(runtime->display);
  return result == H2_PAL_OK ? closed : result;
}
