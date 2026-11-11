#include "h2_g99_screen.h"
#include "h2_g99_screen_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

#define MAX_SIDE 480

/* ---------- fake panel: records every board draw ---------- */

typedef struct panel {
  int width;
  int height;
  int draws;
  int presents;
  h2_display_rect_t last;
  uint16_t pixels[MAX_SIDE * MAX_SIDE];
} panel_t;

static panel_t s_panel;

static int panel_ok(void* user) {
  (void)user;
  return H2_PAL_OK;
}

static int panel_info(void* user, h2_display_info_t* info) {
  const panel_t* p = user;
  memset(info, 0, sizeof(*info));
  info->width = p->width;
  info->height = p->height;
  return H2_PAL_OK;
}

static int panel_draw(void* user, const h2_display_rect_t* rect,
                      const void* pixels, size_t stride,
                      h2_display_pixel_format_t format) {
  panel_t* p = user;
  if (format != H2_DISPLAY_PIXEL_RGB565 || rect->x < 0 || rect->y < 0 ||
      rect->x + rect->width > p->width || rect->y + rect->height > p->height)
    return H2_PAL_ERR_INVALID_ARG;
  for (int y = 0; y < rect->height; ++y)
    memcpy(&p->pixels[(rect->y + y) * p->width + rect->x],
           (const uint8_t*)pixels + (size_t)y * stride,
           (size_t)rect->width * sizeof(uint16_t));
  ++p->draws;
  p->last = *rect;
  return H2_PAL_OK;
}

static int panel_present(void* user) {
  panel_t* p = user;
  ++p->presents;
  return H2_PAL_OK;
}

static int panel_brightness(void* user, uint32_t percent) {
  (void)user;
  (void)percent;
  return H2_PAL_OK;
}

static const h2_pal_display_vtable_t kPanelVtable = {
    panel_ok, panel_info, panel_draw, panel_present, panel_brightness, panel_ok,
};
static const h2_pal_display_api_t kPanel = {&s_panel, &kPanelVtable};

/* ---------- memory: libc, or none at all ---------- */

static void* mem_alloc(void* user, size_t len) {
  (void)user;
  return malloc(len);
}

static void* mem_realloc(void* user, void* ptr, size_t len) {
  (void)user;
  return realloc(ptr, len);
}

static void mem_free(void* user, void* ptr) {
  (void)user;
  free(ptr);
}

static void* mem_none(void* user, size_t len) {
  (void)user;
  (void)len;
  return NULL;
}

static const h2_pal_mem_vtable_t kMemVtable = {
    .alloc = mem_alloc, .realloc = mem_realloc, .free = mem_free};
static const h2_pal_mem_vtable_t kNoMemVtable = {
    .alloc = mem_none, .realloc = mem_realloc, .free = mem_free};
static const h2_pal_mem_api_t kMem = {.user = NULL, .vtable = &kMemVtable};
static const h2_pal_mem_api_t kNoMem = {.user = NULL, .vtable = &kNoMemVtable};

/* ---------- fake touch: one panel point, reported once ---------- */

static h2_pal_touch_event_t s_touch_event;

static h2_pal_result_t touch_ok(void* user) {
  (void)user;
  return H2_PAL_OK;
}

static h2_pal_result_t touch_info(void* user, h2_pal_touch_info_t* out) {
  (void)user;
  out->width = (uint32_t)s_panel.width;
  out->height = (uint32_t)s_panel.height;
  return H2_PAL_OK;
}

static h2_pal_result_t touch_poll(void* user, h2_pal_touch_event_t* out) {
  (void)user;
  *out = s_touch_event;
  return H2_PAL_OK;
}

static const h2_pal_touch_vtable_t kTouchVtable = {touch_ok, touch_info,
                                                   touch_poll, touch_ok};
static const h2_pal_touch_api_t kTouch = {NULL, &kTouchVtable};

/* ---------- helpers ---------- */

static h2_g99_screen_t s_screen;
static uint16_t s_frame[MAX_SIDE * MAX_SIDE];

/* Distinct under transposition and mirroring of a coordinate. */
static uint16_t value(int x, int y) {
  return (uint16_t)(x * 977 + y * 131 + 1);
}

/* Logical (x, y) shows at panel (y, H - 1 - x). */
static uint16_t shown(int x, int y) {
  return s_panel.pixels[(s_panel.height - 1 - x) * s_panel.width + y];
}

static int count_wrong(int x0, int y0, int w, int h, int offset) {
  int wrong = 0;
  for (int y = y0; y < y0 + h; ++y)
    for (int x = x0; x < x0 + w; ++x)
      if (shown(x, y) != (uint16_t)(value(x, y) + offset)) ++wrong;
  return wrong;
}

static void fill_frame(int w, int h) {
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) s_frame[y * w + x] = value(x, y);
}

static void setup(int panel_w, int panel_h) {
  h2_g99_screen_deinit(&s_screen);
  memset(&s_panel, 0, sizeof(s_panel));
  s_panel.width = panel_w;
  s_panel.height = panel_h;
  CHECK(h2_g99_screen_init(&s_screen, &kPanel, &kTouch, &kMem,
                           H2_G99_SCREEN_ROTATION) == H2_PAL_OK);
  CHECK(h2_pal_display_open(h2_g99_screen_display(&s_screen)) == H2_PAL_OK);
}

/* LVGL renders 480 x 48 bands; they reach the board as one draw of whole
 * panel rows on present. */
static void test_bands_coalesce(void) {
  setup(480, 480);
  const h2_pal_display_api_t* d = h2_g99_screen_display(&s_screen);
  fill_frame(480, 480);
  for (int y = 0; y < 480; y += 48) {
    const h2_display_rect_t band = {0, y, 480, 48};
    CHECK(h2_pal_display_draw_bitmap(d, &band, &s_frame[y * 480], 480 * 2,
                                     H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
  }
  CHECK(s_panel.draws == 0);
  CHECK(h2_pal_display_present(d) == H2_PAL_OK);
  CHECK(s_panel.draws == 1 && s_panel.presents == 1);
  CHECK(s_panel.last.x == 0 && s_panel.last.y == 0 &&
        s_panel.last.width == 480 && s_panel.last.height == 480);
  CHECK(count_wrong(0, 0, 480, 480, 0) == 0);
  /* Content turned counter-clockwise: the top-left corner shows bottom left
   * and the top-right corner top left. */
  CHECK(s_panel.pixels[479 * 480 + 0] == value(0, 0));
  CHECK(s_panel.pixels[0] == value(479, 0));
  /* Nothing drawn since: present sends no pixels. */
  CHECK(h2_pal_display_present(d) == H2_PAL_OK);
  CHECK(s_panel.draws == 1 && s_panel.presents == 2);
}

/* A sub-rectangle with a row stride on a non-square panel sends the panel
 * rows of its logical columns only, keeping the rest of those rows. */
static void test_partial_rect(void) {
  setup(6, 4); /* logical 4 x 6 */
  const h2_pal_display_api_t* d = h2_g99_screen_display(&s_screen);
  h2_display_info_t info;
  CHECK(h2_pal_display_get_info(d, &info) == H2_PAL_OK);
  CHECK(info.width == 4 && info.height == 6);
  fill_frame(4, 6);
  const h2_display_rect_t all = {0, 0, 4, 6};
  CHECK(h2_pal_display_draw_bitmap(d, &all, s_frame, 4 * 2,
                                   H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
  CHECK(h2_pal_display_present(d) == H2_PAL_OK);
  CHECK(count_wrong(0, 0, 4, 6, 0) == 0);
  enum { STRIDE = 8 };
  uint16_t rect_pixels[3 * STRIDE];
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 2; ++x)
      rect_pixels[y * STRIDE + x] = (uint16_t)(value(1 + x, 2 + y) + 1000);
  const h2_display_rect_t rect = {1, 2, 2, 3};
  CHECK(h2_pal_display_draw_bitmap(d, &rect, rect_pixels, STRIDE * 2,
                                   H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
  CHECK(h2_pal_display_present(d) == H2_PAL_OK);
  /* Logical columns 1 and 2 are panel rows 2 and 1. */
  CHECK(s_panel.draws == 2);
  CHECK(s_panel.last.x == 0 && s_panel.last.y == 1 && s_panel.last.width == 6 &&
        s_panel.last.height == 2);
  CHECK(count_wrong(1, 2, 2, 3, 1000) == 0);
  CHECK(count_wrong(1, 0, 2, 2, 0) == 0);
  CHECK(count_wrong(1, 5, 2, 1, 0) == 0);
  CHECK(count_wrong(0, 0, 1, 6, 0) == 0);
  CHECK(count_wrong(3, 0, 1, 6, 0) == 0);
  /* Outside the logical screen, or another format, is refused. */
  const h2_display_rect_t outside = {3, 0, 2, 1};
  CHECK(h2_pal_display_draw_bitmap(d, &outside, rect_pixels, STRIDE * 2,
                                   H2_DISPLAY_PIXEL_RGB565) ==
        H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_display_draw_bitmap(d, &rect, rect_pixels, STRIDE * 2,
                                   H2_DISPLAY_PIXEL_RGB888) ==
        H2_PAL_ERR_UNSUPPORTED);
}

/* Without memory for the frame the display does not open. */
static void test_no_memory(void) {
  memset(&s_panel, 0, sizeof(s_panel));
  s_panel.width = 4;
  s_panel.height = 4;
  h2_g99_screen_t screen;
  CHECK(h2_g99_screen_init(&screen, &kPanel, NULL, &kNoMem,
                           H2_G99_SCREEN_ROTATION) == H2_PAL_OK);
  const h2_pal_display_api_t* d = h2_g99_screen_display(&screen);
  CHECK(h2_pal_display_open(d) == H2_PAL_ERR_NO_MEMORY);
  const h2_display_rect_t rect = {0, 0, 1, 1};
  const uint16_t pixel = 1u;
  CHECK(h2_pal_display_draw_bitmap(d, &rect, &pixel, sizeof(pixel),
                                   H2_DISPLAY_PIXEL_RGB565) ==
        H2_PAL_ERR_INVALID_STATE);
  h2_g99_screen_deinit(&screen);
}

static void test_stride_overflow(void) {
  setup(6, 4);
  const h2_display_rect_t rect = {0, 0, 1, 2};
  const uint16_t pixel = 1u;
  CHECK(h2_pal_display_draw_bitmap(h2_g99_screen_display(&s_screen), &rect,
                                   &pixel, SIZE_MAX, H2_DISPLAY_PIXEL_RGB565) ==
        H2_PAL_ERR_INVALID_ARG);
}

static void test_touch(void) {
  setup(6, 4);
  const h2_pal_touch_api_t* t = h2_g99_screen_touch(&s_screen);
  h2_pal_touch_info_t info;
  CHECK(h2_pal_touch_get_info(t, &info) == H2_PAL_OK);
  CHECK(info.width == 4u && info.height == 6u);
  /* Panel (5, 0) shows logical (3, 5): touching it reports (3, 5). */
  s_touch_event = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 5, 0};
  h2_pal_touch_event_t event;
  CHECK(h2_pal_touch_poll_event(t, &event) == H2_PAL_OK);
  CHECK(event.kind == H2_PAL_TOUCH_EVENT_DOWN && event.x == 3 && event.y == 5);
  s_touch_event = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, 0, 3};
  CHECK(h2_pal_touch_poll_event(t, &event) == H2_PAL_OK);
  CHECK(event.x == 0 && event.y == 0);
  /* Missing board APIs stay missing. */
  h2_g99_screen_t empty;
  CHECK(h2_g99_screen_init(&empty, NULL, NULL, &kMem, H2_G99_SCREEN_ROTATION) ==
        H2_PAL_OK);
  CHECK(h2_g99_screen_display(&empty) == NULL);
  CHECK(h2_g99_screen_touch(&empty) == NULL);
}

static void test_unrotated(void) {
  h2_g99_screen_t screen;
  CHECK(h2_g99_screen_init(&screen, &kPanel, &kTouch, NULL,
                           H2_G99_SCREEN_ROTATION_NONE) == H2_PAL_OK);
  const h2_pal_display_api_t* display = h2_g99_screen_display(&screen);
  CHECK(h2_pal_display_open(display) == H2_PAL_OK);
  h2_display_info_t display_info;
  CHECK(h2_pal_display_get_info(display, &display_info) == H2_PAL_OK);
  CHECK(display_info.width == s_panel.width &&
        display_info.height == s_panel.height);
  const h2_display_rect_t rect = {1, 1, 1, 1};
  const uint16_t pixel = 419u;
  CHECK(h2_pal_display_draw_bitmap(display, &rect, &pixel, sizeof(pixel),
                                   H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
  CHECK(h2_pal_display_present(display) == H2_PAL_OK);
  CHECK(s_panel.pixels[s_panel.width + 1] == pixel);
  s_touch_event = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_DOWN, 2, 1};
  h2_pal_touch_event_t event;
  CHECK(h2_pal_touch_poll_event(h2_g99_screen_touch(&screen), &event) ==
        H2_PAL_OK);
  CHECK(event.x == 2 && event.y == 1);
  CHECK(h2_pal_display_close(display) == H2_PAL_OK);
  CHECK(screen.frame == NULL);
  h2_g99_screen_deinit(&screen);
  CHECK(h2_g99_screen_init(&screen, &kPanel, &kTouch, &kMem,
                           (h2_g99_screen_rotation_t)180) ==
        H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
  test_bands_coalesce();
  test_partial_rect();
  test_no_memory();
  test_stride_overflow();
  test_touch();
  test_unrotated();
  h2_g99_screen_deinit(&s_screen);
  if (failures != 0) return 1;
  (void)printf("PASS\n");
  return 0;
}
