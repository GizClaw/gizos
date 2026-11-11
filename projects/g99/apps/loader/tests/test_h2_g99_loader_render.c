/* Renders the Loader screen in every state at 480 x 480 and 320 x 240 into a
 * memory display, checks the frames and writes them as BMP files to
 * TEST_UNDECLARED_OUTPUTS_DIR when Bazel provides it. */

#include "hal/h2_pal_display.h"
#include "h2_desktop_platform.h"
#include "h2_g99_art.h"
#include "h2_g99_brand.h"
#include "h2_g99_loader_ui.h"

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

/* ---------- memory display ---------- */

typedef struct screen {
  int32_t width;
  int32_t height;
  int open;
  uint16_t pixels[MAX_SIDE * MAX_SIDE];
} screen_t;

static int screen_open(void* user) {
  ((screen_t*)user)->open = 1;
  return H2_PAL_OK;
}

static int screen_info(void* user, h2_display_info_t* info) {
  const screen_t* s = user;
  memset(info, 0, sizeof(*info));
  info->width = (uint32_t)s->width;
  info->height = (uint32_t)s->height;
  return H2_PAL_OK;
}

static int screen_draw(void* user, const h2_display_rect_t* rect,
                       const void* pixels, size_t stride_bytes,
                       h2_display_pixel_format_t format) {
  screen_t* s = user;
  if (format != H2_DISPLAY_PIXEL_RGB565 || rect->x < 0 || rect->y < 0 ||
      rect->x + rect->width > s->width || rect->y + rect->height > s->height)
    return H2_PAL_ERR_INVALID_ARG;
  const uint8_t* bytes = pixels;
  for (int32_t y = 0; y < rect->height; ++y)
    memcpy(&s->pixels[(rect->y + y) * s->width + rect->x],
           bytes + (size_t)y * stride_bytes,
           (size_t)rect->width * sizeof(uint16_t));
  return H2_PAL_OK;
}

static int screen_present(void* user) {
  (void)user;
  return H2_PAL_OK;
}

static int screen_close(void* user) {
  ((screen_t*)user)->open = 0;
  return H2_PAL_OK;
}

static const h2_pal_display_vtable_t kScreenVtable = {
    .open = screen_open,
    .get_info = screen_info,
    .draw_bitmap = screen_draw,
    .present = screen_present,
    .close = screen_close,
};

/* ---------- helpers ---------- */

static uint16_t rgb565(uint32_t rgb) {
  return (uint16_t)(((rgb >> 8) & 0xF800u) | ((rgb >> 5) & 0x07E0u) |
                    ((rgb >> 3) & 0x001Fu));
}

static screen_t s_screen;
static uint16_t s_brand_frame[MAX_SIDE * MAX_SIDE];

static uint16_t at(int32_t x, int32_t y) {
  return s_screen.pixels[y * s_screen.width + x];
}

/* Non-black pixels in rows [y0, y1). */
static int lit_rows(int32_t y0, int32_t y1) {
  int lit = 0;
  for (int32_t y = y0; y < y1; ++y)
    for (int32_t x = 0; x < s_screen.width; ++x)
      if (at(x, y) != 0u) ++lit;
  return lit;
}

static void write_bmp(const char* name) {
  const char* dir = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (dir == NULL || dir[0] == '\0') return;
  const int32_t w = s_screen.width;
  const int32_t h = s_screen.height;
  char path[512];
  (void)snprintf(path, sizeof(path), "%s/%dx%d-%s.bmp", dir, (int)w, (int)h,
                 name);
  FILE* file = fopen(path, "wb");
  if (file == NULL) {
    ++failures;
    return;
  }
  const uint32_t row = ((uint32_t)w * 3u + 3u) & ~3u;
  const uint32_t fields[][2] = {{2u, 54u + row * (uint32_t)h},
                                {10u, 54u},
                                {14u, 40u},
                                {18u, (uint32_t)w},
                                {22u, (uint32_t)h},
                                {34u, row * (uint32_t)h}};
  uint8_t header[54] = {'B', 'M'};
  for (size_t f = 0u; f < sizeof(fields) / sizeof(fields[0]); ++f)
    for (uint32_t i = 0u; i < 4u; ++i)
      header[fields[f][0] + i] = (uint8_t)(fields[f][1] >> (8u * i));
  header[26] = 1u;
  header[28] = 24u;
  (void)fwrite(header, 1u, sizeof(header), file);
  static uint8_t line[MAX_SIDE * 3 + 4];
  for (int32_t y = h - 1; y >= 0; --y) {
    memset(line, 0, sizeof(line));
    for (int32_t x = 0; x < w; ++x) {
      const uint16_t p = at(x, y);
      line[x * 3] = (uint8_t)((p & 0x1Fu) * 255u / 31u);
      line[x * 3 + 1] = (uint8_t)(((p >> 5) & 0x3Fu) * 255u / 63u);
      line[x * 3 + 2] = (uint8_t)(((p >> 11) & 0x1Fu) * 255u / 31u);
    }
    (void)fwrite(line, 1u, row, file);
  }
  (void)fclose(file);
}

/* ---------- the run ---------- */

static h2_g99_loader_ui_t* open_ui(h2_runtime_t* runtime,
                                   h2_pal_display_api_t* display, int32_t width,
                                   int32_t height) {
  memset(&s_screen, 0, sizeof(s_screen));
  s_screen.width = width;
  s_screen.height = height;
  display->user = &s_screen;
  display->vtable = &kScreenVtable;
  memset(runtime, 0, sizeof(*runtime));
  runtime->mem = h2_desktop_platform_default_allocator();
  runtime->task = h2_desktop_platform_task_api();
  runtime->sync = h2_desktop_platform_sync_api();
  runtime->queue = h2_desktop_platform_queue_api();
  runtime->time = h2_desktop_platform_time_api();
  runtime->display = display;
  h2_g99_loader_ui_t* ui = NULL;
  CHECK(h2_g99_loader_ui_open(runtime, &ui) == H2_PAL_OK);
  return ui;
}

/* Through one padding below the brand group every frame equals the
 * brand-only frame. (Status ascenders may rise a little above status_top,
 * which the design estimated from Montserrat's cap height.) */
static void check_brand_unchanged(const h2_g99_brand_layout_t* l) {
  const int32_t end = l->wordmark_y + l->wordmark_image_h + l->status_x;
  for (int32_t y = 0; y < end; ++y)
    for (int32_t x = 0; x < l->width; ++x)
      if (at(x, y) != s_brand_frame[y * l->width + x]) {
        (void)fprintf(stderr, "brand area changed at %d,%d (status_top %d)\n",
                      (int)x, (int)y, (int)l->status_top);
        ++failures;
        return;
      }
}

static void check_bar(const h2_g99_brand_layout_t* l, uint32_t percent,
                      uint32_t fill_rgb) {
  const int32_t fill = (l->width * (int32_t)percent * 2 + 100) / 200;
  const int32_t y = l->bar_y + l->bar_h / 2;
  CHECK(fill > 0 && fill < l->width);
  CHECK(at(fill - 1, y) == rgb565(fill_rgb));
  CHECK(at(fill, y) == rgb565(H2_G99_COLOR_DEEP));
  CHECK(at(0, l->height - 1) == rgb565(fill_rgb));
  CHECK(at(l->width - 1, l->bar_y) == rgb565(H2_G99_COLOR_DEEP));
  CHECK(at(l->width - 1, l->bar_y - 1) == 0u);
}

static void run_size(int32_t width, int32_t height) {
  h2_runtime_t runtime;
  h2_pal_display_api_t display;
  h2_g99_loader_ui_t* ui = open_ui(&runtime, &display, width, height);
  if (ui == NULL) return;
  h2_g99_brand_layout_t l;
  CHECK(h2_g99_brand_layout(width, height, &l));
  CHECK(s_screen.open == 1);

  /* Brand alone: lit pixels only inside the brand group. */
  write_bmp("1-brand");
  memcpy(s_brand_frame, s_screen.pixels,
         (size_t)width * (size_t)height * sizeof(uint16_t));
  CHECK(lit_rows(0, l.group_y) == 0);
  CHECK(lit_rows(l.group_y, l.wordmark_y + l.wordmark_image_h) > 0);
  CHECK(lit_rows(l.wordmark_y + l.wordmark_image_h, height) == 0);
  CHECK(at(l.icon_x + l.icon_w / 2, l.group_y + l.icon_h - 2) ==
        rgb565(H2_G99_COLOR_PURPLE));

  CHECK(h2_g99_loader_ui_ready(ui) == H2_PAL_OK);
  write_bmp("2-ready");
  check_brand_unchanged(&l);
  CHECK(lit_rows(l.status_top, l.bar_y) > 0);
  CHECK(lit_rows(l.bar_y, height) == 0);

  CHECK(h2_g99_loader_ui_installing(ui) == H2_PAL_OK);
  write_bmp("3-installing");
  check_brand_unchanged(&l);
  CHECK(lit_rows(l.bar_y, height) == 0);

  /* 49 % of the image phase is 41 % overall. */
  CHECK(h2_g99_loader_ui_progress(ui, H2_LOADER_INSTALL_PHASE_IMAGE, 49u,
                                  100u) == H2_PAL_OK);
  write_bmp("4-writing-image");
  check_brand_unchanged(&l);
  check_bar(&l, 41u, H2_G99_COLOR_PURPLE);

  CHECK(h2_g99_loader_ui_progress(ui, H2_LOADER_INSTALL_PHASE_VERIFY, 3u,
                                  10u) == H2_PAL_OK);
  write_bmp("5-verifying");
  check_bar(&l, 92u, H2_G99_COLOR_PURPLE);

  CHECK(h2_g99_loader_ui_failed(ui, -5) == H2_PAL_OK);
  write_bmp("6-failed");
  check_brand_unchanged(&l);
  check_bar(&l, 92u, H2_G99_COLOR_ERROR);

  CHECK(h2_g99_loader_ui_installing(ui) == H2_PAL_OK);
  CHECK(h2_g99_loader_ui_launching(ui) == H2_PAL_OK);
  write_bmp("7-starting");
  check_brand_unchanged(&l);
  CHECK(lit_rows(l.status_top, l.bar_y) > 0);
  CHECK(lit_rows(l.bar_y, height) == 0);
  h2_g99_loader_ui_close(ui);
  CHECK(s_screen.open == 0);

  /* A normal boot launches with the brand alone, identical to power-on. */
  ui = open_ui(&runtime, &display, width, height);
  if (ui == NULL) return;
  CHECK(h2_g99_loader_ui_launching(ui) == H2_PAL_OK);
  h2_g99_loader_state_t state;
  h2_g99_loader_ui_state(ui, &state);
  CHECK(state.screen == H2_G99_LOADER_SCREEN_BRAND);
  CHECK(memcmp(s_screen.pixels, s_brand_frame,
               (size_t)width * (size_t)height * sizeof(uint16_t)) == 0);
  h2_g99_loader_ui_close(ui);
}

int main(void) {
  run_size(480, 480);
  run_size(320, 240);
  if (failures != 0) return 1;
  (void)printf("PASS\n");
  return 0;
}
