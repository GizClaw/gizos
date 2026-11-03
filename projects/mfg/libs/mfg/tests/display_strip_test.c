#include "../src/h2_mfg_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 240, HEIGHT = 320, GUARD = 0xa55a };

typedef struct fixture {
  uint16_t* output;
  uint16_t* allocation;
  size_t allocated;
  unsigned rows, draws, presents, services;
  int next_y, fail_y, service_error_at;
} fixture_t;

static void* allocate(void* user, size_t bytes) {
  fixture_t* f = user;
  assert(f->allocation == NULL);
  assert(bytes == (size_t)WIDTH * f->rows * sizeof(uint16_t));
  f->allocation = malloc(bytes + 2u * sizeof(uint16_t));
  assert(f->allocation != NULL);
  f->allocated = bytes;
  f->allocation[0] = f->allocation[bytes / 2u + 1u] = GUARD;
  return f->allocation + 1;
}

static void release(void* user, void* pointer) {
  fixture_t* f = user;
  assert(pointer == f->allocation + 1);
  assert(f->allocation[0] == GUARD);
  assert(f->allocation[f->allocated / 2u + 1u] == GUARD);
  free(f->allocation);
  f->allocation = NULL;
}

static int open_display(void* user) {
  (void)user;
  return 0;
}

static int info(void* user, h2_display_info_t* out) {
  (void)user;
  *out = (h2_display_info_t){WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565};
  return 0;
}

static int brightness(void* user, uint32_t percent) {
  (void)user;
  assert(percent == 100);
  return 0;
}

static int draw(void* user, const h2_display_rect_t* area, const void* pixels,
                size_t stride, h2_display_pixel_format_t format) {
  fixture_t* f = user;
  assert(area->x == 0 && area->y == f->next_y && area->width == WIDTH);
  assert(area->height > 0 && area->height <= (int)f->rows);
  assert(area->height <= HEIGHT - area->y);
  assert(stride == WIDTH * sizeof(uint16_t));
  assert(format == H2_DISPLAY_PIXEL_RGB565 && pixels != NULL);
  ++f->draws;
  if (area->y == f->fail_y) return H2_PAL_ERR_IO;
  memcpy(f->output + (size_t)area->y * WIDTH, pixels,
         (size_t)area->height * stride);
  f->next_y += area->height;
  return 0;
}

static int present(void* user) {
  fixture_t* f = user;
  assert(f->next_y == HEIGHT);
  ++f->presents;
  return 0;
}

static const h2_pal_display_vtable_t display_vtable = {
    .open = open_display,
    .get_info = info,
    .draw_bitmap = draw,
    .present = present,
    .close = open_display,
    .set_brightness_percent = brightness,
};
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate,
                                               .free = release};
static const h2_mfg_step_t steps[] = {
    {"display", H2_MFG_DISPLAY, 0}, {"buttons", H2_MFG_BUTTONS, 0},
    {"camera", H2_MFG_CAMERA, 0},   {"mic", H2_MFG_MIC, 0},
    {"aec", H2_MFG_AEC, 0},         {"battery", H2_MFG_BATTERY, 0},
    {"power", H2_MFG_POWER, 0},
};
static const h2_mfg_ui_config_t factory_ui = {
    .width = WIDTH,
    .height = HEIGHT,
    .title = "Factory",
    .background = 0x0862,
    .accent = 0x153f,
    .text = 0xffff,
    .passed = 0x07e0,
    .failed = 0xf800,
    .muted = 0x4208,
    .disabled = 0x3186,
    .title_area = {12, 8, 216, 26},
    .heading = {12, 40, 216, 30},
    .progress = {12, 80, 216, 8},
    .controls = {{12, 270, 100, 38}, {128, 270, 100, 38}},
    .display_pattern = {12, 100, 216, 80},
    .display_note = {12, 190, 216, 30},
    .rows = {{12, 90, 216, 30},
             {12, 120, 216, 30},
             {12, 150, 216, 30},
             {12, 180, 216, 30},
             {12, 210, 216, 30},
             {12, 240, 216, 30},
             {12, 240, 216, 30},
             {12, 90, 216, 30}},
    .preview = {25, 103, 160, 90},
};

static int service(h2_mfg_t* m) {
  fixture_t* f = m->rt->mem->user;
  assert(f->draws == f->services + 1 && f->presents == 0);
  ++f->services;
  /* Servicing audio updates live meters without tearing one displayed frame. */
  m->model.ready = !m->model.ready;
  m->mic_level += 512;
  ++m->audio_frames;
  return (int)f->services == f->service_error_at ? H2_PAL_ERR_IO : 0;
}

static void render(uint16_t* output, unsigned rows, unsigned check, int factory,
                   int fail_y, int service_error_at) {
  fixture_t f = {
      .output = output, .rows = rows == 0 ? HEIGHT : rows, .fail_y = fail_y,
      .service_error_at = service_error_at};
  h2_pal_mem_api_t mem = {.user = &f, .vtable = &mem_vtable};
  h2_pal_display_api_t display = {.user = &f, .vtable = &display_vtable};
  h2_runtime_t rt = {.mem = &mem, .display = &display};
  h2_mfg_profile_t profile = {.board = "test",
                              .revision = 1,
                              .steps = steps,
                              .total = sizeof(steps) / sizeof(steps[0]),
                              .buttons = {103, 102, 101},
                              .button_names = {"OK", "UP", "DOWN"},
                              .button_count = 3,
                              .display_buffer_rows = rows,
                              .ui = factory ? &factory_ui : NULL};
  h2_mfg_t m = {
      .rt = &rt,
      .profile = &profile,
      .model = {.profile = &profile, .summary = {.total = profile.total}}};
  assert(h2_mfg_hardware_enter(&m, 0) == 0);
  assert(m.frame_rows == f.rows);
  m.model.current = check;
  m.model.ready = 1;
  memset(m.model.summary.step_status, H2_LOADER_MFG_STEP_PASSED, profile.total);
  uint16_t preview[32 * 16];
  for (unsigned i = 0; i < 32u * 16u; ++i) preview[i] = (uint16_t)(i * 97u);
  m.camera_preview = preview;
  m.preview_width = 32;
  m.preview_height = 16;
  m.mic_level = 1024;
  m.battery.voltage_mv = 3800;
  m.battery.percent_x100 = 7500;
  m.render_service = service;
  int rc = h2_mfg_ui_draw(&m, 2000);
  assert(rc == (fail_y < 0 ? 0 : H2_PAL_ERR_IO));
  assert(m.frame_y == 0);
  if (service_error_at > 0) {
    assert(m.render_service_error == H2_PAL_ERR_IO);
    assert(f.services == (unsigned)service_error_at && f.draws == f.services);
    assert(f.presents == 0);
  } else if (fail_y < 0) {
    assert(f.services == f.draws);
    assert(f.draws == (HEIGHT + f.rows - 1u) / f.rows);
    assert(f.presents == 1);
  } else {
    assert(f.draws == (unsigned)fail_y / f.rows + 1u);
    assert(f.services + 1 == f.draws);
    assert(f.presents == 0);
  }
  m.camera_preview = NULL;
  assert(h2_mfg_hardware_close(&m) == 0);
  assert(f.allocation == NULL);
}

int main(void) {
  size_t bytes = WIDTH * HEIGHT * sizeof(uint16_t);
  uint16_t* reference = malloc(bytes);
  uint16_t* actual = malloc(bytes);
  assert(reference != NULL && actual != NULL);
  for (int factory = 0; factory < 2; ++factory) {
    for (unsigned check = 0; check <= sizeof(steps) / sizeof(steps[0]);
         ++check) {
      render(reference, 0, check, factory, -1, 0);
      const unsigned sizes[] = {1, 4, 7, 8, 127};
      for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        memset(actual, 0xa5, bytes);
        render(actual, sizes[i], check, factory, -1, 0);
        assert(memcmp(reference, actual, bytes) == 0);
      }
    }
    render(actual, 8, 0, factory, 16, 0);
    render(actual, 4, 0, factory, -1, 3);
  }
  free(reference);
  free(actual);
  return 0;
}
