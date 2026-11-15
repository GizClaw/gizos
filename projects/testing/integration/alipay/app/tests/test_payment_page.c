#include "h2_alipay_payment_page.h"
#include "h2_desktop_platform.h"
#include "h2_smoke_host_runtime.h"
#include "h2_lvgl_platform.h"
#include "h2_lvgl_display.h"
#include "h2_runtime_test.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct fixture {
  pthread_t ui;
  unsigned phase, cycles, touch_opens, touch_closes;
  atomic_uint codes, queries, starts, stops, scans, scan_stops;
  bool scanning, hardware_buttons, production_input, payer_only;
  unsigned navigation_phase;
  atomic_uint pressed_button, group_reads;
  h2_runtime_t* runtime;
  int touch_rc;
  h2_runtime_test_control_t* input;
  uint16_t pixels[240 * 240];
} fixture_t;
static int open_display(void* user) {
  (void)user;
  return 0;
}
static int display_info(void* user, h2_display_info_t* out) {
  (void)user;
  *out = (h2_display_info_t){240, 240, H2_DISPLAY_PIXEL_RGB565};
  return 0;
}
static int draw(void* user, const h2_display_rect_t* rect, const void* bytes,
                size_t stride, h2_display_pixel_format_t format) {
  fixture_t* f = user;
  assert(pthread_equal(f->ui, pthread_self()));
  assert(format == H2_DISPLAY_PIXEL_RGB565);
  for (int y = 0; y < rect->height; ++y)
    memcpy(f->pixels + (rect->y + y) * 240 + rect->x,
           (const uint8_t*)bytes + y * stride, (size_t)rect->width * 2u);
  return 0;
}
static int present(void* user) {
  (void)user;
  return 0;
}
static int brightness(void* user, uint32_t value) {
  (void)user;
  (void)value;
  return 0;
}
static int close_display(void* user) {
  (void)user;
  return 0;
}
static int touch_open(void* user) {
  fixture_t* f = user;
  ++f->touch_opens;
  return f->touch_rc;
}
static int touch_info(void* user, h2_pal_touch_info_t* out) {
  (void)user;
  *out = (h2_pal_touch_info_t){240, 240};
  return 0;
}
static int touch_poll(void* user, h2_pal_touch_event_t* out) {
  (void)user;
  (void)out;
  return H2_PAL_ERR_WOULD_BLOCK;
}
static int touch_close(void* user) {
  ++((fixture_t*)user)->touch_closes;
  return 0;
}
static void worker(fixture_t* f) {
  assert(!pthread_equal(f->ui, pthread_self()));
}
static int code(void* user, uint8_t* out, uint32_t* length, int* sdk) {
  fixture_t* f = user;
  worker(f);
  assert(*length >= 20u);
  unsigned count = atomic_fetch_add(&f->codes, 1u);
  memcpy(out, count == 0u ? "123456789012345678" : "987654321098765432", 19u);
  *length = 18u;
  *sdk = 0;
  return 0;
}
static int query(void* user, const char* code,
                 h2_alipay_payment_result_t* out) {
  fixture_t* f = user;
  worker(f);
  assert(strcmp(code, "123456789012345678") == 0 ||
         strcmp(code, "987654321098765432") == 0);
  atomic_fetch_add(&f->queries, 1u);
  *out = (h2_alipay_payment_result_t){.state = H2_ALIPAY_PAYMENT_PENDING};
  return 0;
}
static int tap_start(void* user, const h2_alipay_tap_transport_t* transport,
                     h2_alipay_payment_result_t* out) {
  fixture_t* f = user;
  worker(f);
  assert(transport->broadcast_direct != NULL);
  atomic_fetch_add(&f->starts, 1u);
  *out = (h2_alipay_payment_result_t){0};
  return 0;
}
static int stop(void* user) {
  fixture_t* f = user;
  worker(f);
  atomic_fetch_add(&f->stops, 1u);
  return 0;
}
static int host_start(void* user) {
  worker(user);
  return 0;
}
static int scan_start(void* user, const h2_pal_ble_scan_params_t* params,
                      h2_pal_ble_scan_result_fn callback, void* context) {
  fixture_t* f = user;
  worker(f);
  assert(params && callback && context && !f->scanning);
  f->scanning = true;
  atomic_fetch_add(&f->scans, 1u);
  return 0;
}
static int scan_stop(void* user) {
  fixture_t* f = user;
  worker(f);
  assert(f->scanning);
  unsigned calls = atomic_fetch_add(&f->scan_stops, 1u);
  if (calls == 0u) return H2_PAL_ERR_BUSY;
  f->scanning = false;
  return 0;
}
static lv_obj_t* find_label(lv_obj_t* parent, const char* text) {
  if (lv_obj_check_type(parent, &lv_label_class) &&
      strcmp(lv_label_get_text(parent), text) == 0)
    return parent;
  uint32_t count = lv_obj_get_child_count(parent);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t* result = find_label(lv_obj_get_child(parent, (int32_t)i), text);
    if (result) return result;
  }
  return NULL;
}
static bool click(const char* text) {
  lv_obj_t* label = find_label(lv_screen_active(), text);
  if (label == NULL) return false;
  lv_obj_t* button = lv_obj_get_parent(label);
  if (lv_obj_has_state(button, LV_STATE_DISABLED) || lv_obj_is_hidden(button))
    return false;
  assert(lv_obj_send_event(button, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
  return true;
}
static void save_frame(fixture_t* f, const char* name) {
  const char* directory = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (directory == NULL) return;
  char path[4096];
  assert(snprintf(path, sizeof(path), "%s/%s.ppm", directory, name) > 0);
  FILE* file = fopen(path, "wb");
  assert(file != NULL);
  fprintf(file, "P6\n240 240\n255\n");
  for (size_t i = 0u; i < 240u * 240u; ++i) {
    uint16_t pixel = f->pixels[i];
    const uint8_t rgb[] = {(uint8_t)(((pixel >> 11u) & 31u) * 255u / 31u),
                           (uint8_t)(((pixel >> 5u) & 63u) * 255u / 63u),
                           (uint8_t)((pixel & 31u) * 255u / 31u)};
    assert(fwrite(rgb, 1u, 3u, file) == 3u);
  }
  assert(fclose(file) == 0);
}
static void key(fixture_t* f, h2_runtime_component_id_t id) {
  if (f->production_input) {
    /* Drive the public Radio Button PAL; Runtime owns acquisition and events.
     */
    for (unsigned edge = 0u; edge < 2u; ++edge) {
      atomic_store(&f->pressed_button, edge == 0u ? id + 100u : 0u);
      bool observed = false;
      for (unsigned tries = 0u; tries < 200u; ++tries) {
        h2_runtime_button_state_t state;
        assert(h2_runtime_component_state_button(f->runtime, id, &state) == 0);
        if (state.pressed == (edge == 0u)) {
          observed = true;
          break;
        }
        assert(h2_pal_time_sleep_ms(f->runtime->time, 2u) == 0);
      }
      assert(observed);
    }
    return;
  }
  uint64_t now = (uint64_t)f->cycles * 100u + 1000u;
  /* A held action must not click; only the final release activates once. */
  assert(h2_runtime_test_button_action(f->input, id, now, 0u, now) == 0);
  assert(h2_runtime_test_button_action(f->input, id, now, now + 1u, now + 1u) ==
         0);
}
static bool activate(fixture_t* f, const char* text) {
  if (!f->hardware_buttons) return click(text);
  lv_obj_t* label = find_label(lv_screen_active(), text);
  if (label == NULL) return false;
  lv_obj_t* button = lv_obj_get_parent(label);
  if (lv_obj_has_state(button, LV_STATE_DISABLED) || lv_obj_is_hidden(button))
    return false;
  if (strcmp(text, "Back") == 0) {
    key(f, 5u);
    return true;
  }
  lv_group_t* group = lv_obj_get_group(button);
  assert(group != NULL);
  if (lv_group_get_focused(group) != button) {
    key(f, 3u);
    return false;
  }
  key(f, 4u);
  return true;
}
static int mapping_list(void* user, h2_runtime_component_t kind,
                        h2_runtime_component_mapping_cb_t callback,
                        void* callback_user) {
  (void)user;
  if (kind != H2_RUNTIME_COMPONENT_BUTTON) return 0;
  for (uint32_t id = 2u; id <= 5u; ++id) {
    const h2_runtime_component_mapping_entry_t entry = {id, id + 100u};
    int rc = callback(callback_user, &entry);
    if (rc != 0) return rc;
  }
  return 0;
}
static int periph_get(void* user, h2_pal_periph_id_t id,
                      h2_pal_periph_info_t* out) {
  fixture_t* f = user;
  if (id < 102u || id > 105u) return H2_PAL_ERR_NOT_FOUND;
  static const h2_pal_periph_single_button_payload_t payload = {
      .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE};
  static const h2_pal_periph_radio_button_payload_t radio = {.group_id = 100u};
  *out = (h2_pal_periph_info_t){
      .id = id,
      .type = f->production_input ? H2_PAL_PERIPH_TYPE_RADIO_BUTTON
                                  : H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
      .name = "test button",
      .payload =
          f->production_input ? (const void*)&radio : (const void*)&payload,
      .payload_size = f->production_input ? sizeof(radio) : sizeof(payload)};
  return 0;
}
static int periph_list(void* user, h2_pal_periph_type_t kind,
                       h2_pal_periph_cb_t callback, void* callback_user) {
  fixture_t* f = user;
  h2_pal_periph_type_t type = f->production_input
                                  ? H2_PAL_PERIPH_TYPE_RADIO_BUTTON
                                  : H2_PAL_PERIPH_TYPE_SINGLE_BUTTON;
  if (kind != H2_PAL_PERIPH_TYPE_ANY && kind != type) return 0;
  for (uint32_t id = 102u; id <= 105u; ++id) {
    h2_pal_periph_info_t info;
    assert(periph_get(user, id, &info) == 0);
    int rc = callback(callback_user, &info);
    if (rc != 0) return rc;
  }
  return 0;
}
static int read_radio(void* user, h2_pal_periph_id_t id,
                      h2_pal_radio_button_group_reading_t* out) {
  fixture_t* f = user;
  assert(id == 100u);
  atomic_fetch_add(&f->group_reads, 1u);
  *out = (h2_pal_radio_button_group_reading_t){
      .id = id, .pressed_button_id = atomic_load(&f->pressed_button)};
  return 0;
}
static bool exit_requested(void* user) {
  fixture_t* f = user;
  assert(pthread_equal(f->ui, pthread_self()));
  assert(++f->cycles < 2000u);
  if (f->hardware_buttons && !f->payer_only && f->navigation_phase < 3u) {
    lv_obj_t* pay =
        lv_obj_get_parent(find_label(lv_screen_active(), "Pay code"));
    lv_obj_t* tap =
        lv_obj_get_parent(find_label(lv_screen_active(), "Tap to pay"));
    lv_group_t* group = lv_obj_get_group(pay);
    lv_obj_t* expected = f->navigation_phase == 1u ? tap : pay;
    assert(lv_group_get_focused(group) == expected);
    /* Inspect actual rendered pixels without any keypad indev or touch. */
    lv_refr_now(lv_display_get_default());
    uint16_t pay_color = f->pixels[80u * 240u + 30u];
    uint16_t tap_color = f->pixels[136u * 240u + 30u];
    uint16_t selected = (uint16_t)(((0x14u >> 3u) << 11u) |
                                   ((0x56u >> 2u) << 5u) | (0xb8u >> 3u));
    uint16_t other = (uint16_t)(((0xe5u >> 3u) << 11u) | ((0xe7u >> 2u) << 5u) |
                                (0xebu >> 3u));
    if (pay_color != (f->navigation_phase == 1u ? other : selected) ||
        tap_color != (f->navigation_phase == 1u ? selected : other))
      fprintf(stderr,
              "focus phase=%u pay=%04x tap=%04x selected=%04x other=%04x\n",
              f->navigation_phase, pay_color, tap_color, selected, other);
    save_frame(f, "physical-focus-diagnostic");
    assert(pay_color == (f->navigation_phase == 1u ? other : selected));
    assert(tap_color == (f->navigation_phase == 1u ? selected : other));
    if (f->navigation_phase == 0u) {
      save_frame(f, "physical-menu-pay-focused");
      key(f, 3u);
    } else if (f->navigation_phase == 1u) {
      save_frame(f, "physical-menu-tap-focused");
      key(f, 2u);
    }
    ++f->navigation_phase;
    return false;
  }
  if (f->phase == 0u) save_frame(f, "payment-menu");
  if (f->phase == 1u && find_label(lv_screen_active(), "Show code to merchant"))
    save_frame(f, "payer-qr-controlled");
  if (f->phase == 0u && activate(f, "Pay code"))
    ++f->phase;
  else if (f->phase == 1u &&
           find_label(lv_screen_active(), "Show code to merchant") &&
           activate(f, "Refresh"))
    ++f->phase;
  else if (f->phase == 2u && atomic_load(&f->codes) == 2u &&
           find_label(lv_screen_active(), "Show code to merchant") &&
           activate(f, "Back"))
    ++f->phase;
  else if (f->phase == 3u && f->payer_only &&
           find_label(lv_screen_active(), "Choose payment method")) {
    lv_obj_t* tap = find_label(lv_screen_active(), "Tap to pay");
    assert(tap != NULL && lv_obj_is_hidden(lv_obj_get_parent(tap)));
    f->phase = 6u;
    return true;
  } else if (f->phase == 3u &&
             find_label(lv_screen_active(), "Choose payment method") &&
             activate(f, "Tap to pay"))
    ++f->phase;
  else if (f->phase == 4u &&
           find_label(lv_screen_active(), "Tap the payment terminal") &&
           activate(f, "Back"))
    ++f->phase;
  else if (f->phase == 5u &&
           find_label(lv_screen_active(), "Choose payment method")) {
    ++f->phase;
    return true;
  }
  return false;
}
static void run(bool buttons, bool production, bool payer_only, int touch_rc,
                int expected) {
  fixture_t f = {.ui = pthread_self(),
                 .hardware_buttons = buttons,
                 .production_input = production,
                 .payer_only = payer_only,
                 .touch_rc = touch_rc};
  const h2_pal_display_vtable_t dvt = {
      open_display, display_info, draw, present, brightness, close_display};
  h2_pal_display_api_t display = {&f, &dvt};
  const h2_pal_touch_vtable_t tvt = {touch_open, touch_info, touch_poll,
                                     touch_close};
  const h2_pal_touch_api_t touch = {&f, &tvt};
  const h2_pal_ble_vtable_t bvt = {
      .start = host_start, .start_scan = scan_start, .stop_scan = scan_stop};
  const h2_pal_ble_api_t ble = {.user = &f, .vtable = &bvt};
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "test", "host", "host", h2_desktop_platform_default_allocator(),
      h2_desktop_platform_time_api(), h2_desktop_platform_queue_api(),
      &display);
  config.touch = &touch;
  config.ble_host = &ble;
  config.sync = h2_desktop_platform_sync_api();
  config.task = h2_desktop_platform_task_api();
  const h2_pal_button_vtable_t buttons_vt = {.read_radio_button_group =
                                                 read_radio};
  const h2_pal_button_api_t buttons_api = {.user = &f, .vtable = &buttons_vt};
  const h2_runtime_component_mapper_vtable_t mvt = {.list = mapping_list};
  const h2_runtime_component_mapper_t mapper = {.vtable = &mvt};
  const h2_pal_periph_vtable_t periph_vt = {.list = periph_list,
                                            .get = periph_get};
  const h2_pal_periph_api_t periph = {.user = &f, .vtable = &periph_vt};
  if (buttons) {
    config.component_mapper = &mapper;
    config.periph = &periph;
    config.button = &buttons_api;
  }
  h2_runtime_t* rt = NULL;
  int initialized = h2_runtime_init(&config, &rt);
  if (initialized != 0)
    fprintf(stderr, "runtime init buttons=%d rc=%d\n", buttons, initialized);
  assert(initialized == 0);
  f.runtime = rt;
  if (production) {
    assert(h2_runtime_input_start(rt, NULL) == 0);
  } else if (buttons)
    assert(h2_runtime_test_control_open(rt, &f.input) == 0);
  const h2_lvgl_platform_config_t platform = {.allocator = rt->mem,
                                              .time_api = rt->time,
                                              .task_api = rt->task,
                                              .sync_api = rt->sync,
                                              .queue_api = rt->queue};
  assert(h2_lvgl_platform_init(&platform) == 0);
  lv_init();
  h2_lvgl_display_t* view = NULL;
  const h2_lvgl_display_config_t dc = {
      .display = &display, .allocator = rt->mem, .draw_buffer_rows = 240u};
  assert(h2_lvgl_display_create(&dc, &view) == 0);
  const h2_alipay_payment_vtable_t pvt = {
      .get_code = code,
      .query_code = query,
      .start_tap = payer_only ? NULL : tap_start,
      .stop_tap = stop};
  const h2_alipay_payment_api_t payment = {&f, &pvt};
  const h2_alipay_e2e_config_t app = {
      .payment = &payment,
      .user = &f,
      .should_exit = exit_requested,
      .payment_buttons = buttons ? (h2_alipay_payment_buttons_t){2u, 3u, 4u, 5u}
                                 : (h2_alipay_payment_buttons_t){0}};
  assert(h2_alipay_payment_page_run(rt, &app, h2_lvgl_display_lvgl(view), 240,
                                    240, NULL, NULL) == expected);
  if (expected == 0) {
    assert(f.phase == 6u && atomic_load(&f.codes) == 2u &&
           atomic_load(&f.starts) == (payer_only ? 0u : 1u));
    assert(atomic_load(&f.scans) == (payer_only ? 0u : 1u) &&
           atomic_load(&f.scan_stops) == (payer_only ? 0u : 2u) && !f.scanning);
  } else {
    assert(f.phase == 0u && atomic_load(&f.codes) == 0u &&
           atomic_load(&f.starts) == 0u);
  }
  assert(f.touch_opens == 1u && f.touch_closes == (touch_rc == 0 ? 1u : 0u));
  assert(lv_group_get_count() == 0u);
  h2_lvgl_display_destroy(view);
  lv_deinit();
  h2_lvgl_platform_deinit();
  if (production) {
    assert(h2_runtime_input_stop(rt) == 0);
    assert(atomic_load(&f.group_reads) > 10u);
  }
  if (f.input != NULL) h2_runtime_test_control_close(f.input);
  assert(h2_runtime_destroy(&rt) == 0);
}
int main(void) {
  run(false, false, false, 0, 0);
  run(false, false, false, H2_PAL_ERR_UNSUPPORTED, H2_PAL_ERR_UNSUPPORTED);
  run(true, false, false, H2_PAL_ERR_UNSUPPORTED, 0);
  run(true, false, false, H2_PAL_ERR_IO, H2_PAL_ERR_IO);
  run(true, true, false, H2_PAL_ERR_UNSUPPORTED, 0);
  run(true, true, true, H2_PAL_ERR_UNSUPPORTED, 0);
  return 0;
}
