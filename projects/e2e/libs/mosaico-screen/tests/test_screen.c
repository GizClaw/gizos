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
static int log_fault, log_calls;
static char last_log[H2_PAL_LOG_MESSAGE_MAX];
static int log_write(void *user, h2_pal_log_level_t level, const char *scope,
                     const char *message) {
  (void)user;
  assert(level == H2_PAL_LOG_INFO && strcmp(scope, "mosaico-screen") == 0);
  assert(strlen(message) < sizeof(last_log));
  strcpy(last_log, message);
  ++log_calls;
  return log_fault ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static const h2_pal_log_vtable_t log_vtable = {.write = log_write};
static const h2_pal_log_api_t log_api = {.vtable = &log_vtable};

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
  assert(h2_mosaico_e2e_screen_begin(NULL, &log_api, "DISPLAY", 1) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(h2_mosaico_e2e_screen_begin(&panel, &log_api, NULL, 1) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(h2_mosaico_e2e_screen_begin(&panel, &log_api, "DISPLAY", 20) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(h2_mosaico_e2e_screen_begin(&panel, NULL, "DISPLAY", 1) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  fault = 1;
  assert(h2_mosaico_e2e_screen_begin(&panel, &log_api, "DISPLAY", 1) ==
         H2_DISPLAY_ERR_IO);
  fault = 0;
  width = 320;
  assert(h2_mosaico_e2e_screen_begin(&panel, &log_api, "DISPLAY", 1) ==
         H2_DISPLAY_ERR_UNSUPPORTED);
  width = 480;
  assert(h2_mosaico_e2e_screen_begin(&panel, &log_api, "DISPLAY", 1) == 0);
  assert(draw_calls == 20 && presents == 1 && colored(0xffffu) > 0u &&
         colored(0x7dffu) > 0u);
  int before = opens;
  assert(h2_mosaico_e2e_screen_phase("TEST PATTERN / DIMMING") == 0 &&
         opens == before);
  assert(h2_mosaico_e2e_screen_phase(NULL) == H2_DISPLAY_ERR_INVALID_ARG);
  assert(h2_mosaico_e2e_screen_result(0, 0) == 0 && colored(0x7fefu) > 0u &&
         colored(0xfaaau) == 0u);
  assert(strcmp(last_log, "H2_MOSAICO_SCREEN suite=DISPLAY stage=PASS rc=0") ==
         0);
  assert(log_calls > 0);
  log_fault = 1;
  assert(h2_mosaico_e2e_screen_phase("LOG FAILURE") == H2_PAL_ERR_IO);
  log_fault = 0;
  char oversized[H2_PAL_LOG_MESSAGE_MAX + 1];
  memset(oversized, 'X', sizeof(oversized) - 1u);
  oversized[sizeof(oversized) - 1u] = '\0';
  assert(h2_mosaico_e2e_screen_phase(oversized) == H2_PAL_ERR_INVALID_ARG);
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
  assert(h2_mosaico_e2e_screen_begin(&panel, &log_api, "DISPLAY", 1) ==
         H2_DISPLAY_ERR_UNSUPPORTED);
  assert(h2_mosaico_e2e_screen_release() == H2_DISPLAY_ERR_IO);
  width = 480;
  fault = 0;
  assert(h2_mosaico_e2e_screen_phase("RETRY") == 0 && opens == before + 1);
  assert(h2_mosaico_e2e_screen_release() == 0);
  fault = 6;
  assert(h2_mosaico_e2e_screen_begin(&panel, &log_api, "DISPLAY", 1) ==
         H2_DISPLAY_ERR_IO);
  fault = 0;
  assert(h2_mosaico_e2e_screen_release() == 0);
  assert(h2_mosaico_e2e_screen_begin(&panel, &log_api, "GIZCLAW", 19) == 0);
  assert(h2_mosaico_e2e_screen_release() == 0);
  puts("Screen bounds, actual rendered status, failures and ownership/reopen "
       "PASS");
  return 0;
}
