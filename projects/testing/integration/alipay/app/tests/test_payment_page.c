#include "h2_alipay_payment_page.h"
#include "h2_desktop_platform.h"
#include "h2_smoke_host_runtime.h"
#include "h2_lvgl_platform.h"
#include "h2_lvgl_display.h"
#include "h2_runtime_test.h"
#include "h2_qrcode.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct fixture {
  pthread_t ui;
  unsigned phase, cycles, touch_opens, touch_closes, host_starts;
  atomic_uint codes, queries, starts, stops, scans, scan_stops;
  bool scanning, hardware_buttons, production_input, payer_only;
  unsigned navigation_phase;
  atomic_uint pressed_button, group_reads;
  h2_runtime_t* runtime;
  int touch_rc, nfc_read_rc;
  h2_runtime_test_control_t* input;
  uint16_t pixels[240 * 240];
  unsigned flow, flow_phase, reads, writes, frames, confirmations;
  unsigned results, advertising_starts, advertising_stops;
  bool advertising;
  h2_alipay_tap_transport_t transport;
  h2_pal_ble_scan_result_fn scan_callback;
  void* scan_user;
  const h2_pal_ble_gatt_service_t* service;
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
  unsigned count = atomic_fetch_add(&f->queries, 1u);
  if (f->flow == 4u) {
    if (count == 0u) {
      *out = (h2_alipay_payment_result_t){.sdk_rc = 99};
      return H2_PAL_ERR_IO;
    }
    if (count == 1u)
      assert(strcmp(code, "987654321098765432") == 0);
    else
      assert(strcmp(code, "123456789012345678") == 0);
    *out = (h2_alipay_payment_result_t){.state = count == 1u
                                                     ? H2_ALIPAY_PAYMENT_PENDING
                                                     : H2_ALIPAY_PAYMENT_PAID,
                                        .amount = "0.01"};
    return 0;
  }
  *out = (h2_alipay_payment_result_t){.state = H2_ALIPAY_PAYMENT_PENDING};
  return 0;
}
static int tap_start(void* user, const h2_alipay_tap_transport_t* transport,
                     h2_alipay_payment_result_t* out) {
  fixture_t* f = user;
  worker(f);
  assert(transport->write_reply != NULL && atomic_load(&f->scans) == 0u);
  f->transport = *transport;
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
  fixture_t* fixture = user;
  worker(fixture);
  ++fixture->host_starts;
  return 0;
}
static int scan_start(void* user, const h2_pal_ble_scan_params_t* params,
                      h2_pal_ble_scan_result_fn callback, void* context) {
  fixture_t* f = user;
  worker(f);
  assert(params && callback && context && !f->scanning);
  f->scanning = true;
  f->scan_callback = callback;
  f->scan_user = context;
  atomic_fetch_add(&f->scans, 1u);
  return 0;
}

static int read_nfc(void* user, h2_pal_periph_id_t id, const uint8_t* uid,
                    uint8_t uid_length, h2_pal_nfc_data_type_t type,
                    const h2_pal_mem_api_t* memory,
                    h2_pal_nfc_data_read_t* out) {
  fixture_t* fixture = user;
  worker(fixture);
  assert(id == 101u && uid_length == 4u && uid[0] == 0x42u &&
         type == H2_PAL_NFC_DATA_NDEF);
  ++fixture->reads;
  if (fixture->flow == 8u && fixture->reads == 1u) return H2_PAL_ERR_IO;
  if (fixture->flow == 12u) {
    if (fixture->reads <= 4u || fixture->reads == 6u || fixture->reads == 7u)
      return H2_PAL_ERR_BUSY;
    if (fixture->reads == 5u) return H2_PAL_ERR_IO;
  }
  if (fixture->flow == 13u)
    return fixture->reads == 1u ? H2_PAL_ERR_IO : H2_PAL_ERR_BUSY;
  if (fixture->flow == 15u && fixture->reads <= 10u) return H2_PAL_ERR_BUSY;
  if (fixture->flow == 16u) {
    if (fixture->reads <= 2u) return H2_PAL_ERR_IO;
    if (fixture->reads <= 8u) return H2_PAL_ERR_BUSY;
  }
  if (fixture->flow == 17u) {
    if (fixture->reads == 1u) return H2_PAL_ERR_IO;
    if (fixture->reads <= 101u) return H2_PAL_ERR_BUSY;
  }
  if (fixture->nfc_read_rc != 0) return fixture->nfc_read_rc;
  out->bytes = h2_pal_mem_alloc(memory, 4u);
  assert(out->bytes != NULL);
  memcpy(out->bytes, "TEST", 4u);
  out->len = 4u;
  if (fixture->flow == 11u && fixture->reads == 1u) return H2_PAL_ERR_IO;
  return 0;
}

static int encode_reply(void* user, const uint8_t* data, size_t length,
                        uint8_t* out, size_t capacity, size_t* size) {
  fixture_t* fixture = user;
  worker(fixture);
  assert(length == 4u && memcmp(data, "TEST", 4u) == 0 && capacity >= 8u);
  const uint8_t record[] = {0xd1u, 1u, 4u, 'T', 0u, 'O', 'K', '!'};
  memcpy(out, record, sizeof(record));
  *size = sizeof(record);
  return 0;
}

static int write_nfc(void* user, h2_pal_periph_id_t id, const uint8_t* uid,
                     uint8_t uid_length, h2_pal_nfc_data_type_t type,
                     const uint8_t* data, size_t length) {
  fixture_t* fixture = user;
  worker(fixture);
  assert(id == 101u && uid_length == 4u && uid[0] == 0x42u &&
         type == H2_PAL_NFC_DATA_NDEF);
  const uint8_t record[] = {0xd1u, 1u, 4u, 'T', 0u, 'O', 'K', '!'};
  assert(length == sizeof(record) && memcmp(data, record, length) == 0);
  ++fixture->writes;
  return fixture->flow == 7u ? H2_PAL_ERR_IO : 0;
}

static int receive_ndef(void* user, const uint8_t* data, size_t length,
                        h2_alipay_payment_result_t* out) {
  fixture_t* fixture = user;
  worker(fixture);
  assert(length == 4u && memcmp(data, "TEST", 4u) == 0);
  assert(fixture->transport.write_reply != NULL);
  int rc =
      fixture->transport.write_reply(fixture->transport.user, data, length);
  *out = (h2_alipay_payment_result_t){
      .write_attempted = true, .nfc_write_rc = rc, .response_sent = rc == 0};
  return rc;
}

static int query_tap(void* user, h2_alipay_payment_result_t* out) {
  fixture_t* fixture = user;
  worker(fixture);
  if (fixture->flow == 6u || fixture->flow == 8u || fixture->flow == 11u ||
      fixture->flow == 12u || fixture->flow == 16u || fixture->flow == 17u) {
    ++fixture->results;
    assert(fixture->writes == 1u);
    *out = (h2_alipay_payment_result_t){.state = H2_ALIPAY_PAYMENT_PAID,
                                        .amount = "0.00"};
    return 0;
  }
  *out = (h2_alipay_payment_result_t){.state = H2_ALIPAY_PAYMENT_PENDING};
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

static void verify_payer_qr(fixture_t* fixture, const char* expected) {
  uint8_t modules[H2_QRCODE_BUFFER_LEN_FOR_VERSION(10)];
  uint8_t scratch[sizeof(modules)];
  h2_qrcode_t code;
  assert(h2_qrcode_encode_text(expected, H2_QRCODE_ECC_QUARTILE, 10, modules,
                               sizeof(modules), scratch, sizeof(scratch),
                               &code) == 0);
  h2_qrcode_layout_t layout;
  assert(h2_qrcode_layout_center(&code, 224, 140, 4, &layout) == 0);
  int span = (code.size + 8) * layout.scale;
  int left = (240 - span) / 2;
  lv_refr_now(lv_display_get_default());
  for (int row = 0; row < span; ++row)
    for (int column = 0; column < span; ++column) {
      int x = column / layout.scale - 4;
      int y = row / layout.scale - 4;
      uint16_t pixel = h2_qrcode_module_is_dark(&code, x, y) ? 0u : UINT16_MAX;
      assert(fixture->pixels[(size_t)(32 + row) * 240u + left + column] ==
             pixel);
    }
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
  fixture_t* fixture = user;
  if (fixture->flow != 0u && kind == H2_RUNTIME_COMPONENT_NFC_READER) {
    const h2_runtime_component_mapping_entry_t entry = {1u, 101u};
    return callback(callback_user, &entry);
  }
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
  if (id == 101u && f->flow != 0u) {
    *out = (h2_pal_periph_info_t){
        .id = id, .type = H2_PAL_PERIPH_TYPE_NFC_READER, .name = "test nfc"};
    return 0;
  }
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
  if (f->flow != 0u && (kind == H2_PAL_PERIPH_TYPE_ANY ||
                        kind == H2_PAL_PERIPH_TYPE_NFC_READER)) {
    h2_pal_periph_info_t info;
    assert(periph_get(user, 101u, &info) == 0);
    int rc = callback(callback_user, &info);
    if (rc != 0) return rc;
  }
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
static bool flow_exit(fixture_t* fixture) {
  if (fixture->flow == 4u) {
    char failure[96];
    (void)snprintf(failure, sizeof(failure),
                   "Payment query failed\nSDK: 99 / rc: %d", H2_PAL_ERR_IO);
    if (fixture->flow_phase == 0u && click("Pay code"))
      fixture->flow_phase = 1u;
    else if (fixture->flow_phase == 1u &&
             find_label(lv_screen_active(), failure)) {
      verify_payer_qr(fixture, "123456789012345678");
      lv_obj_t* screen = lv_screen_active();
      lv_obj_t* canvas = NULL;
      for (uint32_t index = 0u; index < lv_obj_get_child_count(screen);
           ++index) {
        lv_obj_t* child = lv_obj_get_child(screen, (int32_t)index);
        if (lv_obj_check_type(child, &lv_canvas_class)) canvas = child;
      }
      assert(canvas != NULL && lv_obj_is_clickable(canvas));
      assert(lv_obj_send_event(canvas, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
      fixture->flow_phase = 2u;
    } else if (fixture->flow_phase == 2u &&
               find_label(lv_screen_active(), "Waiting for merchant result")) {
      verify_payer_qr(fixture, "987654321098765432");
      fixture->flow_phase = 3u;
    } else if (fixture->flow_phase == 3u &&
               find_label(lv_screen_active(), "Payment confirmed\n0.01"))
      fixture->flow_phase = 6u;
    return fixture->flow_phase == 6u;
  }
  if (fixture->flow_phase == 0u && click("Tap to pay"))
    fixture->flow_phase = 1u;
  else if (fixture->flow_phase == 1u &&
           find_label(lv_screen_active(), "Tap the payment terminal")) {
    const h2_runtime_nfc_state_t state = {
        .status = H2_RUNTIME_NFC_STATE_DISCOVERED,
        .stage = H2_PAL_NFC_STAGE_DISCOVERED,
        .uid_len = 4u,
        .uid = {0x42u, 1u, 2u, 3u}};
    assert(h2_runtime_test_set_component_state(fixture->input, 1u, &state,
                                               sizeof(state)) == 0);
    fixture->flow_phase = 2u;
  } else if (fixture->flow_phase == 2u && fixture->flow == 7u) {
    char failure[80];
    (void)snprintf(failure, sizeof(failure), "NFC write failed\nrc: %d",
                   H2_PAL_ERR_IO);
    if (find_label(lv_screen_active(), failure) != NULL) {
      assert(fixture->writes == 1u && fixture->results == 0u &&
             fixture->advertising_starts == 0u);
      fixture->flow_phase = 6u;
    }
  } else if ((fixture->flow_phase == 2u || fixture->flow_phase == 4u) &&
             (fixture->flow == 6u || fixture->flow == 8u ||
              fixture->flow == 11u || fixture->flow == 12u ||
              fixture->flow == 16u || fixture->flow == 17u)) {
    if (find_label(lv_screen_active(), "Response sent; checking payment") !=
        NULL)
      fixture->flow_phase = 4u;
    if (find_label(lv_screen_active(), "Payment confirmed\n0.00") != NULL)
      fixture->flow_phase = 6u;
  } else if (fixture->flow_phase == 2u &&
             (fixture->flow == 5u || fixture->flow == 9u ||
              fixture->flow == 10u || fixture->flow == 13u ||
              fixture->flow == 14u || fixture->flow == 15u)) {
    char failure[80];
    (void)snprintf(failure, sizeof(failure), "NFC read failed\nrc: %d",
                   fixture->nfc_read_rc);
    if (find_label(lv_screen_active(), failure) != NULL) {
      unsigned calls = fixture->flow == 9u    ? 3u
                       : fixture->flow == 13u ? 102u
                       : fixture->flow == 14u ? 101u
                       : fixture->flow == 15u ? 11u
                                              : 1u;
      assert(fixture->reads == calls && fixture->writes == 0u &&
             fixture->frames == 0u && fixture->confirmations == 0u &&
             fixture->results == 0u);
      fixture->flow_phase = 6u;
    }
  }
  return fixture->flow_phase == 6u;
}

static bool payer_visible(void) {
  return find_label(lv_screen_active(), "Show code to merchant") != NULL ||
         find_label(lv_screen_active(), "Waiting for merchant result") != NULL;
}

static bool exit_requested(void* user) {
  fixture_t* f = user;
  assert(pthread_equal(f->ui, pthread_self()));
  assert(++f->cycles < 2000u);
  if (f->flow != 0u) return flow_exit(f);
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
  if (f->phase == 1u && payer_visible())
    verify_payer_qr(f, "123456789012345678");
  if (f->phase == 0u && activate(f, "Pay code"))
    ++f->phase;
  else if (f->phase == 1u && payer_visible() && activate(f, "Refresh"))
    ++f->phase;
  else if (f->phase == 2u && atomic_load(&f->codes) == 2u && payer_visible() &&
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
                int expected, unsigned flow) {
  fixture_t f = {.ui = pthread_self(),
                 .hardware_buttons = buttons,
                 .production_input = production,
                 .payer_only = payer_only,
                 .flow = flow,
                 .nfc_read_rc = flow == 5u    ? H2_PAL_ERR_NO_MEMORY
                                : flow == 9u  ? H2_PAL_ERR_IO
                                : flow == 13u ? H2_PAL_ERR_IO
                                : flow == 14u ? H2_PAL_ERR_BUSY
                                : flow == 10u || flow == 15u
                                    ? H2_PAL_ERR_INVALID_STATE
                                    : 0,
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
  const h2_pal_nfc_vtable_t nfc_vtable = {.read_nfc_data = read_nfc,
                                          .write_nfc_data = write_nfc};
  const h2_pal_nfc_api_t nfc = {.user = &f, .vtable = &nfc_vtable};
  config.nfc = &nfc;
  const h2_pal_button_vtable_t buttons_vt = {.read_radio_button_group =
                                                 read_radio};
  const h2_pal_button_api_t buttons_api = {.user = &f, .vtable = &buttons_vt};
  const h2_runtime_component_mapper_vtable_t mvt = {.list = mapping_list};
  const h2_runtime_component_mapper_t mapper = {.user = &f, .vtable = &mvt};
  const h2_pal_periph_vtable_t periph_vt = {.list = periph_list,
                                            .get = periph_get};
  const h2_pal_periph_api_t periph = {.user = &f, .vtable = &periph_vt};
  if (buttons || flow != 0u) {
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
  } else if (buttons || flow != 0u)
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
      .receive_ndef = receive_ndef,
      .query_tap = query_tap,
      .start_tap = payer_only ? NULL : tap_start,
      .stop_tap = stop};
  const h2_alipay_payment_api_t payment = {&f, &pvt};
  const h2_alipay_e2e_config_t app = {
      .payment = &payment,
      .payment_nfc_component_id = 1u,
      .payment_nfc_encode_reply = encode_reply,
      .user = &f,
      .should_exit = exit_requested,
      .payment_buttons = buttons ? (h2_alipay_payment_buttons_t){2u, 3u, 4u, 5u}
                                 : (h2_alipay_payment_buttons_t){0}};
  assert(h2_alipay_payment_page_run(rt, &app, h2_lvgl_display_lvgl(view), 240,
                                    240, NULL, NULL) == expected);
  if (flow == 4u) {
    assert(f.flow_phase == 6u && atomic_load(&f.codes) == 2u &&
           atomic_load(&f.queries) == 3u && f.reads == 0u);
  } else if (flow >= 5u) {
    unsigned expected_reads = flow == 12u                   ? 8u
                              : flow == 13u || flow == 17u  ? 102u
                              : flow == 14u                 ? 101u
                              : flow == 15u                 ? 11u
                              : flow == 16u                 ? 9u
                              : flow == 9u                  ? 3u
                              : (flow == 8u || flow == 11u) ? 2u
                                                            : 1u;
    bool read_failed = flow == 5u || flow == 9u || flow == 10u || flow == 13u ||
                       flow == 14u || flow == 15u;
    assert(f.flow_phase == 6u && f.reads == expected_reads &&
           f.writes == (read_failed ? 0u : 1u) &&
           f.results == (flow == 6u || flow == 8u || flow == 11u ||
                                 flow == 12u || flow == 16u || flow == 17u
                             ? 1u
                             : 0u) &&
           f.confirmations == 0u && f.advertising_starts == 0u &&
           atomic_load(&f.scans) == 0u && atomic_load(&f.scan_stops) == 0u);
  } else if (expected == 0) {
    assert(f.phase == 6u && atomic_load(&f.codes) == 2u &&
           atomic_load(&f.starts) == (payer_only ? 0u : 1u));
    assert(atomic_load(&f.scans) == 0u && atomic_load(&f.scan_stops) == 0u &&
           !f.scanning);
  } else {
    assert(f.phase == 0u && atomic_load(&f.codes) == 0u &&
           atomic_load(&f.starts) == 0u);
  }
  assert(f.host_starts == 0u && atomic_load(&f.scans) == 0u);
  assert(f.touch_opens == 0u && f.touch_closes == 0u);
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
  run(false, false, false, 0, H2_PAL_ERR_INVALID_ARG, 0u);
  run(true, false, false, 0, 0, 0u);
  run(true, false, false, H2_PAL_ERR_UNSUPPORTED, 0, 0u);
  run(true, false, false, H2_PAL_ERR_IO, 0, 0u);
  run(true, true, false, H2_PAL_ERR_UNSUPPORTED, 0, 0u);
  run(true, true, true, H2_PAL_ERR_UNSUPPORTED, 0, 0u);
  for (unsigned flow = 4u; flow <= 7u; ++flow)
    run(true, false, false, 0, 0, flow);
  run(true, false, false, 0, 0, 9u);
  run(true, false, false, 0, 0, 8u);
  run(true, false, false, 0, 0, 10u);
  run(true, false, false, 0, 0, 11u);
  run(true, false, false, 0, 0, 12u);
  run(true, false, false, 0, 0, 13u);
  run(true, false, false, 0, 0, 14u);
  run(true, false, false, 0, 0, 15u);
  run(true, false, false, 0, 0, 16u);
  run(true, false, false, 0, 0, 17u);
  return 0;
}
