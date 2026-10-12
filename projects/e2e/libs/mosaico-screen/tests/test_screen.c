#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_mosaico_e2e_screen.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint16_t pixels[480u * 480u];
static int opens, closes, presents, draw_calls, fault, width = 480;
static int open_panel(void *u) {
  (void)u;
  ++opens;
  return fault == 1 ? H2_DISPLAY_ERR_IO : 0;
}
static int info(void *u, h2_display_info_t *out) {
  (void)u;
  *out = (h2_display_info_t){
      .width = width, .height = 480, .native_format = H2_DISPLAY_PIXEL_RGB565};
  return fault == 6 ? H2_DISPLAY_ERR_IO : 0;
}
static int draw(void *u, const h2_display_rect_t *r, const void *p,
                size_t stride, h2_display_pixel_format_t f) {
  (void)u;
  ++draw_calls;
  assert(r->x >= 0 && r->y >= 0 && r->x + r->width <= 480 &&
         r->y + r->height <= 480);
  assert(f == H2_DISPLAY_PIXEL_RGB565 && stride == (size_t)r->width * 2u);
  if (fault == 2)
    return H2_DISPLAY_ERR_IO;
  for (int y = 0; y < r->height; ++y)
    memcpy(pixels + (r->y + y) * 480 + r->x,
           (const unsigned char *)p + y * stride, (size_t)r->width * 2u);
  return 0;
}
static int present(void *u) {
  (void)u;
  ++presents;
  return fault == 3 ? H2_DISPLAY_ERR_IO : 0;
}
static int brightness(void *u, uint32_t n) {
  (void)u;
  assert(n == 100u);
  return fault == 4 ? H2_DISPLAY_ERR_IO : 0;
}
static int close_panel(void *u) {
  (void)u;
  ++closes;
  return fault == 5 ? H2_DISPLAY_ERR_IO : 0;
}
static const h2_pal_display_vtable_t vt = {.open = open_panel,
                                           .get_info = info,
                                           .draw_bitmap = draw,
                                           .present = present,
                                           .set_brightness_percent = brightness,
                                           .close = close_panel};
static const h2_pal_display_api_t panel = {.vtable = &vt};
static unsigned colored(uint16_t c) {
  unsigned n = 0;
  for (size_t i = 0; i < 480u * 480u; ++i)
    n += pixels[i] == c;
  return n;
}
static void snapshot(void) {
  const char *dir = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (!dir)
    return;
  char path[512];
  snprintf(path, sizeof(path), "%s/screen.ppm", dir);
  FILE *f = fopen(path, "wb");
  assert(f);
  fprintf(f, "P6\n480 480\n255\n");
  for (size_t i = 0; i < 480u * 480u; ++i) {
    uint16_t p = pixels[i];
    unsigned char rgb[3] = {(unsigned char)(((p >> 11) & 31u) * 255u / 31u),
                            (unsigned char)(((p >> 5) & 63u) * 255u / 63u),
                            (unsigned char)((p & 31u) * 255u / 31u)};
    assert(fwrite(rgb, 1, 3, f) == 3);
  }
  assert(fclose(f) == 0);
}
int main(void) {
  assert(h2_mosaico_e2e_screen_phase("STARTING") ==
         H2_DISPLAY_ERR_INVALID_STATE);
  assert(h2_mosaico_e2e_screen_begin(NULL, "DISPLAY", 1) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(h2_mosaico_e2e_screen_begin(&panel, NULL, 1) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(h2_mosaico_e2e_screen_begin(&panel, "DISPLAY", 17) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  fault = 1;
  assert(h2_mosaico_e2e_screen_begin(&panel, "DISPLAY", 1) ==
         H2_DISPLAY_ERR_IO);
  fault = 0;
  width = 320;
  assert(h2_mosaico_e2e_screen_begin(&panel, "DISPLAY", 1) ==
         H2_DISPLAY_ERR_UNSUPPORTED);
  width = 480;
  assert(h2_mosaico_e2e_screen_begin(&panel, "DISPLAY", 1) == 0);
  assert(draw_calls == 20 && presents == 1 && colored(0xffffu) > 0u &&
         colored(0x7dffu) > 0u);
  int before = opens;
  assert(h2_mosaico_e2e_screen_phase("TEST PATTERN / DIMMING") == 0 &&
         opens == before);
  assert(h2_mosaico_e2e_screen_phase(NULL) == H2_DISPLAY_ERR_INVALID_ARG);
  assert(h2_mosaico_e2e_screen_result(0, 0) == 0 && colored(0x7fefu) > 0u &&
         colored(0xfaaau) == 0u);
  snapshot();
  assert(h2_mosaico_e2e_screen_result(0, -7) == 0 && colored(0xfaaau) > 0u &&
         colored(0x7fefu) == 0u);
  assert(h2_mosaico_e2e_screen_result(-4, 0) == 0 && colored(0xfaaau) > 0u);
  for (fault = 2; fault <= 4; ++fault)
    assert(h2_mosaico_e2e_screen_phase("RUNNING") == H2_DISPLAY_ERR_IO);
  fault = 5;
  assert(h2_mosaico_e2e_screen_release() == H2_DISPLAY_ERR_IO);
  fault = 0;
  assert(h2_mosaico_e2e_screen_release() == 0);
  before = closes;
  assert(h2_mosaico_e2e_screen_release() == 0 && closes == before);
  before = opens;
  assert(h2_mosaico_e2e_screen_phase("CONFIRMING") == 0 && opens == before + 1);
  assert(h2_mosaico_e2e_screen_release() == 0);
  fault = 5;
  width = 320;
  before = opens;
  assert(h2_mosaico_e2e_screen_begin(&panel, "DISPLAY", 1) ==
         H2_DISPLAY_ERR_UNSUPPORTED);
  assert(h2_mosaico_e2e_screen_release() == H2_DISPLAY_ERR_IO);
  width = 480;
  fault = 0;
  assert(h2_mosaico_e2e_screen_phase("RETRY") == 0 && opens == before + 1);
  assert(h2_mosaico_e2e_screen_release() == 0);
  fault = 6;
  assert(h2_mosaico_e2e_screen_begin(&panel, "DISPLAY", 1) ==
         H2_DISPLAY_ERR_IO);
  fault = 0;
  assert(h2_mosaico_e2e_screen_release() == 0);
  puts("Screen bounds, actual rendered status, failures and ownership/reopen "
       "PASS");
  return 0;
}
