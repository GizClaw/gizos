#include "h2_mosaico_e2e_screen.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char glyph_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-:/.,%+?=";
static const uint8_t glyphs[][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
    {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
    {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
    {14, 4, 4, 4, 4, 4, 14},      {7, 2, 2, 2, 18, 18, 12},
    {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17},
    {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
    {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},
    {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},
    {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
    {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},
    {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
    {0, 0, 0, 31, 0, 0, 0},       {0, 4, 4, 0, 4, 4, 0},
    {1, 2, 2, 4, 8, 8, 16},       {0, 0, 0, 0, 0, 4, 4},
    {0, 0, 0, 0, 4, 4, 8},        {17, 2, 4, 8, 16, 17, 0},
    {0, 4, 4, 31, 4, 4, 0},       {14, 17, 1, 2, 4, 0, 4},
    {0, 0, 31, 0, 31, 0, 0}};
_Static_assert(sizeof(glyphs) / sizeof(glyphs[0]) == sizeof(glyph_chars) - 1,
               "font mapping mismatch");

static const h2_pal_display_api_t *panel;
static const h2_pal_log_api_t *logger;
static char suite_name[39];
static unsigned suite_position;
static int opened;
static uint16_t stripe[480u * 24u];
enum {
  BG = 0x0841,
  INK = 0xffff,
  ACTIVE = 0x7dff,
  GOOD = 0x7fef,
  BAD = 0xfaaa
};

static int line(unsigned row, const char *text, uint16_t color) {
  if (row >= 20u || text == NULL)
    return H2_DISPLAY_ERR_INVALID_ARG;
  for (size_t i = 0u; i < sizeof(stripe) / sizeof(stripe[0]); ++i)
    stripe[i] = BG;
  for (size_t c = 0u; text[c] != '\0' && c < 38u; ++c) {
    char letter = (char)toupper((unsigned char)text[c]);
    if (letter == '_')
      letter = '-';
    const char *g = strchr(glyph_chars, letter);
    if (g == NULL)
      continue;
    for (unsigned y = 0u; y < 21u; ++y)
      for (unsigned x = 0u; x < 10u; ++x)
        if (glyphs[g - glyph_chars][y / 3u] & (1u << (4u - x / 2u)))
          stripe[(y + 1u) * 480u + 12u + c * 12u + x] = color;
  }
  const h2_display_rect_t rect = {0, (int)row * 24, 480, 24};
  return h2_pal_display_draw_bitmap(
      panel, &rect, stripe, 480u * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
}
static int render(const char *phase, uint16_t color, int show_rc, int test_rc,
                  int confirm_rc) {
  if (panel == NULL)
    return H2_DISPLAY_ERR_INVALID_STATE;
  if (!opened) {
    int rc = h2_pal_display_open(panel);
    if (rc != 0)
      return rc;
    opened = 1;
  }
  h2_display_info_t info = {0};
  int info_rc = h2_pal_display_get_info(panel, &info);
  if (info_rc == 0 && (info.width != 480 || info.height != 480 ||
                       info.native_format != H2_DISPLAY_PIXEL_RGB565))
    info_rc = H2_DISPLAY_ERR_UNSUPPORTED;
  if (info_rc != 0) {
    /* A failed close retains ownership so release/retry can close the same
     * handle. Never pretend it was released and open a second handle. */
    if (h2_pal_display_close(panel) == 0)
      opened = 0;
    return info_rc;
  }
  char order[39], result[39];
  if (suite_position == 0u)
    snprintf(order, sizeof(order), "FIRMWARE LOADER");
  else
    snprintf(order, sizeof(order), "SUITE %02u / 19", suite_position);
  snprintf(result, sizeof(result), "TEST RC %d  CONFIRM RC %d", test_rc,
           confirm_rc);
  int rc = 0;
  for (unsigned row = 0u; row < 20u && rc == 0; ++row) {
    const char *text =
        row == 1u    ? (suite_position == 0u ? "MOSAICO LOADER" : "MOSAICO E2E")
        : row == 3u  ? order
        : row == 5u  ? suite_name
        : row == 8u  ? "CURRENT STAGE"
        : row == 10u ? phase
        : row == 14u && show_rc ? result
        : row == 17u            ? "USB LOGS AVAILABLE"
                                : "";
    rc = line(row, text, row == 10u ? color : INK);
  }
  if (rc == 0)
    rc = h2_pal_display_set_brightness_percent(panel, 100u);
  if (rc == 0)
    rc = h2_pal_display_present(panel);
  return rc;
}
static int observed(const char *stage, int rc) {
  char message[H2_PAL_LOG_MESSAGE_MAX];
  int size = snprintf(message, sizeof(message),
                      "H2_MOSAICO_SCREEN suite=%s stage=%s rc=%d", suite_name,
                      stage, rc);
  int log_rc = size < 0 || (size_t)size >= sizeof(message)
                   ? H2_PAL_ERR_INVALID_ARG
                   : h2_pal_log_write(logger, H2_PAL_LOG_INFO, "mosaico-screen",
                                      message);
  /* Preserve the panel failure; a missing/failed diagnostic sink must also
   * prevent successful presentation admission. */
  return rc != 0 ? rc : log_rc;
}
int h2_mosaico_e2e_screen_begin(const h2_pal_display_api_t *display,
                                const h2_pal_log_api_t *log, const char *suite,
                                unsigned position) {
  if (display == NULL || log == NULL || log->vtable == NULL ||
      log->vtable->write == NULL || suite == NULL || position > 19u)
    return observed("STARTING", H2_DISPLAY_ERR_INVALID_ARG);
  if (opened && panel != display)
    return observed("STARTING", H2_DISPLAY_ERR_INVALID_STATE);
  panel = display;
  logger = log;
  suite_position = position;
  snprintf(suite_name, sizeof(suite_name), "%s", suite);
  return observed("STARTING", render("STARTING", ACTIVE, 0, 0, 0));
}
int h2_mosaico_e2e_screen_phase(const char *phase) {
  if (phase == NULL)
    return observed("INVALID", H2_DISPLAY_ERR_INVALID_ARG);
  return observed(phase, render(phase, ACTIVE, 0, 0, 0));
}
int h2_mosaico_e2e_screen_result(int test_rc, int confirm_rc) {
  const char *verdict = test_rc != 0 || confirm_rc != 0 ? "FAIL" : "PASS";
  return observed(verdict,
                  render(verdict, test_rc != 0 || confirm_rc != 0 ? BAD : GOOD,
                         1, test_rc, confirm_rc));
}
int h2_mosaico_e2e_screen_release(void) {
  if (panel == NULL || !opened)
    return 0;
  int rc = h2_pal_display_close(panel);
  if (rc == 0)
    opened = 0;
  return observed("DISPLAY TEST OWNS PIXELS", rc);
}
