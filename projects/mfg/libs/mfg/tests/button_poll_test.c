#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"
#include "h2_esp_adc_stabilizer.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

enum { WIDTH = 240, HEIGHT = 320, ROWS = 4 };

typedef struct fixture {
  h2_mfg_t mfg;
  h2_runtime_t runtime;
  h2_pal_button_api_t button;
  h2_pal_display_api_t display;
  h2_esp_adc_radio_button_state_t keys;
  h2_loader_mfg_summary_t saved;
  uint64_t now, press_from, press_until;
  unsigned reads, saves, draws, presents;
} fixture_t;

static int open_display(void* user) {
  (void)user;
  return 0;
}
static int display_info(void* user, h2_display_info_t* out) {
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
  assert(area->height > 0 && area->height <= ROWS);
  assert(pixels != NULL && stride == WIDTH * sizeof(uint16_t));
  assert(format == H2_DISPLAY_PIXEL_RGB565);
  ++f->draws;
  f->now += 4;
  return 0;
}
static int present(void* user) {
  fixture_t* f = user;
  ++f->presents;
  return 0;
}
static int read_keys(void* user, h2_pal_periph_id_t id,
                     h2_pal_radio_button_group_reading_t* out) {
  fixture_t* f = user;
  assert(id == 201);
  ++f->reads;
  int pressed = f->now >= f->press_from && f->now < f->press_until;
  *out = (h2_pal_radio_button_group_reading_t){.id = id};
  assert(h2_esp_adc_radio_button_update(
      &f->keys,
      pressed ? H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED
              : H2_ESP_ADC_RADIO_BUTTON_SAMPLE_RELEASED,
      pressed ? 103 : 0, &out->pressed_button_id));
  return 0;
}
static int save(void* user, const h2_loader_mfg_summary_t* summary) {
  fixture_t* f = user;
  ++f->saves;
  f->saved = *summary;
  return 0;
}
static int service(h2_mfg_t* m) {
  fixture_t* f = m->rt->display->user;
  return h2_mfg_hardware_service(m, f->now);
}
static const h2_pal_button_vtable_t button_vtable = {.read_radio_button_group =
                                                         read_keys};
static const h2_pal_display_vtable_t display_vtable = {
    .open = open_display,
    .get_info = display_info,
    .set_brightness_percent = brightness,
    .draw_bitmap = draw,
    .present = present,
    .close = open_display};
static const h2_mfg_step_t steps[] = {
    {"first", H2_MFG_DISPLAY, 0},   {"second", H2_MFG_DISPLAY, 0},
    {"third", H2_MFG_DISPLAY, 0},   {"fourth", H2_MFG_DISPLAY, 0},
    {"fifth", H2_MFG_DISPLAY, 0},   {"sixth", H2_MFG_DISPLAY, 0},
    {"seventh", H2_MFG_DISPLAY, 0}, {"eighth", H2_MFG_DISPLAY, 0},
    {"ninth", H2_MFG_DISPLAY, 0},   {"power", H2_MFG_POWER, 0}};
static const h2_mfg_profile_t profile = {.board = "test",
                                         .revision = 1,
                                         .steps = steps,
                                         .total = 10,
                                         .buttons = {103, 102, 101},
                                         .button_names = {"OK", "UP", "DOWN"},
                                         .button_count = 3,
                                         .button_group = 201,
                                         .display_buffer_rows = ROWS};

static void enter(fixture_t* f) {
  assert(h2_mfg_hardware_enter(&f->mfg, f->now) == 0);
  f->mfg.hardware_ticket = f->mfg.model.ticket;
  const char* action = NULL;
  assert(h2_mfg_hardware_tick(&f->mfg, f->now, &action) == 0 && action == NULL);
  assert(f->mfg.model.ready);
}
static void setup(fixture_t* f) {
  memset(f, 0, sizeof(*f));
  f->now = 1000;
  f->button = (h2_pal_button_api_t){.user = f, .vtable = &button_vtable};
  f->display = (h2_pal_display_api_t){.user = f, .vtable = &display_vtable};
  f->runtime = (h2_runtime_t){.mem = h2_desktop_platform_default_allocator(),
                              .button = &f->button,
                              .display = &f->display};
  f->mfg.rt = &f->runtime;
  f->mfg.profile = &profile;
  h2_loader_mfg_summary_t summary = {.total = profile.total};
  memset(summary.step_status, H2_LOADER_MFG_STEP_PASSED, 9);
  assert(h2_mfg_model_init(&f->mfg.model, &profile, &summary, 7, save, f) == 0);
  assert(f->mfg.model.current == 9);
  enter(f);
  f->mfg.render_service = service;
}
static void render(fixture_t* f) {
  uint32_t ticket = f->mfg.model.ticket;
  unsigned saves = f->saves;
  unsigned status = f->mfg.model.summary.step_status[9];
  assert(h2_mfg_ui_draw(&f->mfg, f->now) == 0);
  /* A strip callback may collect input, but cannot accept or change a check. */
  assert(f->mfg.model.current == 9 && f->mfg.model.ticket == ticket);
  assert(f->mfg.model.summary.step_status[9] == status);
  assert(f->saves == saves);
}
static const char* tick(fixture_t* f) {
  const char* action = NULL;
  f->now += 20;
  assert(h2_mfg_hardware_tick(&f->mfg, f->now, &action) == 0);
  return action;
}

static void short_press_during_slow_frame(unsigned offset) {
  fixture_t f;
  setup(&f);
  f.press_from = f.now + offset;
  f.press_until = f.press_from + 100;
  render(&f);
  assert(f.draws == HEIGHT / ROWS && f.presents == 1);
  assert(f.mfg.downs[0] == 1 && f.mfg.ups[0] == 1);
  const char* action = tick(&f);
  assert(action != NULL && strcmp(action, "confirm") == 0);
  assert(h2_mfg_model_action(&f.mfg.model, action, "power",
                             f.mfg.model.ticket) == 0);
  assert(f.saves == 1 && h2_mfg_model_complete(&f.mfg.model));
  for (unsigned i = 0; i < 10; ++i)
    assert(f.saved.step_status[i] == H2_LOADER_MFG_STEP_PASSED);
  assert(tick(&f) == NULL && f.saves == 1);
  assert(h2_mfg_hardware_close(&f.mfg) == 0);
}
static void bounce_cannot_accept(void) {
  fixture_t f;
  setup(&f);
  f.press_from = f.now + 80;
  f.press_until = f.press_from + 8;
  render(&f);
  assert(tick(&f) == NULL && f.saves == 0 && f.mfg.downs[0] == 0);
  assert(h2_mfg_hardware_close(&f.mfg) == 0);
}
static void pending_press_cannot_cross_ticket(void) {
  fixture_t f;
  setup(&f);
  f.press_from = f.now + 80;
  f.press_until = f.press_from + 100;
  render(&f);
  assert(f.mfg.downs[0] == 1 && f.mfg.ups[0] == 1);
  assert(h2_mfg_model_action(&f.mfg.model, "retry", "power",
                             f.mfg.model.ticket) == 0);
  enter(&f);
  assert(tick(&f) == NULL);
  assert(f.mfg.model.current == 9 && !h2_mfg_model_complete(&f.mfg.model));
  assert(h2_mfg_hardware_close(&f.mfg) == 0);
}
static void held_before_ready_cannot_accept(void) {
  fixture_t f;
  setup(&f);
  f.mfg.model.ready = 0;
  f.press_from = f.now + 80;
  f.press_until = f.now + 400;
  render(&f);
  assert(f.mfg.downs[0] == 1 && f.mfg.ups[0] == 0);
  f.mfg.model.ready = 1;
  render(&f);
  assert(f.mfg.ups[0] == 1 && tick(&f) == NULL && f.saves == 0);
  assert(h2_mfg_hardware_close(&f.mfg) == 0);
}
static void failed_check_short_press_retries(void) {
  fixture_t f;
  setup(&f);
  f.mfg.model.error = H2_PAL_ERR_IO;
  f.mfg.model.summary.step_status[9] = H2_LOADER_MFG_STEP_FAILED;
  f.press_from = f.now + 80;
  f.press_until = f.press_from + 100;
  render(&f);
  const char* action = tick(&f);
  assert(action != NULL && strcmp(action, "retry") == 0);
  assert(h2_mfg_model_action(&f.mfg.model, action, "power",
                             f.mfg.model.ticket) == 0);
  assert(f.mfg.model.current == 9 && f.mfg.model.error == 0);
  assert(f.saved.step_status[9] == H2_LOADER_MFG_STEP_UNTESTED);
  for (unsigned i = 0; i < 9; ++i)
    assert(f.saved.step_status[i] == H2_LOADER_MFG_STEP_PASSED);
  assert(h2_mfg_hardware_close(&f.mfg) == 0);
}
int main(void) {
  for (unsigned offset = 40; offset <= 160; offset += 17)
    short_press_during_slow_frame(offset);
  bounce_cannot_accept();
  pending_press_cannot_cross_ticket();
  held_before_ready_cannot_accept();
  failed_check_short_press_retries();
  return 0;
}
