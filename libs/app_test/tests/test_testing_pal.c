#include "h2_app_test_audio.h"
#include "h2_app_test_audio_fake.h"
#include "h2_app_test_crypto.h"
#include "h2_app_test_display.h"
#include "h2_app_test_fs.h"
#include "h2_app_test_modem.h"
#include "h2_app_test_periph.h"
#include "h2_app_test_power.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_time.h"
#include "h2_app_test_wifi.h"
#include "testing_pal_support.h"
#include <stdio.h>
#include <string.h>
#define OK(x) assert((x) == H2_PAL_OK)
#define FAIL_ONCE(field)                                                       \
  ((field) = (h2_app_test_fault_t){H2_PAL_ERR_IO, 1u, 0u})

static void test_time(void) {
  h2_app_test_time_t t;
  h2_app_test_time_init(&t, 100u);
  uint64_t n = 0;
  assert(h2_pal_time_get_wall_ms(&t.api, &n) == H2_PAL_ERR_UNAVAILABLE);
  OK(h2_pal_time_set_wall_ms(&t.api, 1000u));
  OK(h2_pal_time_sleep_ms(&t.api, 20u));
  OK(h2_pal_time_get_monotonic_us(&t.api, &n));
  assert(n == 120000u);
  OK(h2_pal_time_get_wall_ms(&t.api, &n));
  assert(n == 1020u);
  FAIL_ONCE(t.sleep);
  assert(h2_pal_time_sleep_ms(&t.api, 5u) == H2_PAL_ERR_IO);
  assert(t.monotonic_ms == 120u);
  t.wall_ms = UINT64_MAX;
  assert(h2_app_test_time_advance(&t, 1u) == H2_PAL_ERR_NO_SPACE);
  assert(t.monotonic_ms == 120u);
  h2_app_test_time_init(&t, UINT64_MAX);
  assert(h2_pal_time_get_monotonic_us(&t.api, &n) == H2_PAL_ERR_NO_SPACE);
  assert(n == 0);
}
static bool scan_stop(void *u, const h2_pal_wifi_scan_entry_t *e) {
  assert(e->ssid_len == 3u);
  ++*(unsigned *)u;
  return false;
}
static void test_wifi_modem(void) {
  h2_app_test_wifi_t w;
  h2_app_test_wifi_init(&w);
  h2_pal_wifi_sta_config_t c = {.ssid = "net", .ssid_len = 3};
  assert(h2_pal_wifi_settings_get_saved_sta_config(&w.settings, &c) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(c.ssid_len == 0u);
  c = (h2_pal_wifi_sta_config_t){.ssid = "net", .ssid_len = 3};
  OK(h2_pal_wifi_settings_set_saved_sta_config(&w.settings, &c));
  FAIL_ONCE(w.clear_saved);
  assert(h2_pal_wifi_settings_clear_saved_sta_config(&w.settings) ==
         H2_PAL_ERR_IO);
  assert(w.saved_present);
  OK(h2_pal_wifi_settings_clear_saved_sta_config(&w.settings));
  assert(!w.saved_present);
  FAIL_ONCE(w.connect);
  assert(h2_pal_wifi_sta_connect(&w.api, &c, 10u) == H2_PAL_ERR_IO);
  assert(w.last_connect.ssid_len == 3u);
  OK(h2_pal_wifi_sta_connect(&w.api, &c, 10u));
  assert(w.status.state == H2_PAL_WIFI_STA_STATE_IDLE);
  c.ssid_len = H2_PAL_WIFI_SSID_MAX + 1u;
  assert(h2_pal_wifi_sta_connect(&w.api, &c, 10u) == H2_PAL_ERR_INVALID_ARG);
  w.entries[0] =
      (h2_pal_wifi_scan_entry_t){.ssid = "net", .ssid_len = 3u, .channel = 1};
  w.entries[1] = w.entries[0];
  w.entry_count = 2u;
  unsigned count = 0;
  OK(h2_pal_wifi_sta_scan(&w.api, NULL, scan_stop, &count, 10u));
  assert(count == 1u);
  h2_pal_wifi_scan_request_t r = {.channel = 2};
  count = 0;
  OK(h2_pal_wifi_sta_scan(&w.api, &r, scan_stop, &count, 10u));
  assert(count == 0u);
  w.entry_count = H2_PAL_WIFI_SCAN_MAX_RESULTS + 1u;
  assert(h2_pal_wifi_sta_scan(&w.api, NULL, scan_stop, &count, 0u) ==
         H2_PAL_ERR_FORMAT);
  h2_app_test_modem_t m;
  h2_app_test_modem_init(&m);
  h2_pal_modem_call_request_t call = {.number = "123", .timeout_ms = 40u};
  FAIL_ONCE(m.dial);
  assert(h2_pal_modem_call_dial(&m.api, &call) == H2_PAL_ERR_IO);
  assert(!strcmp(m.last_dial.number, "123"));
  OK(h2_pal_modem_call_answer(&m.api, 20u));
  OK(h2_pal_modem_call_hangup(&m.api, 30u));
  assert(m.last_answer_timeout_ms == 20u && m.last_hangup_timeout_ms == 30u);
  memset(call.number, 'x', sizeof(call.number));
  assert(h2_pal_modem_call_dial(&m.api, &call) == H2_PAL_ERR_INVALID_ARG);
}
static h2_pal_result_t periph_count(void *u, const h2_pal_periph_info_t *i) {
  (void)i;
  ++*(unsigned *)u;
  return H2_PAL_ERR_IO;
}
static void test_devices(void) {
  h2_app_test_power_t p;
  h2_app_test_power_init(&p);
  FAIL_ONCE(p.reboot);
  assert(h2_pal_power_reboot(&p.api, 42u) == H2_PAL_ERR_IO);
  assert(p.state == H2_PAL_POWER_STATE_RUNNING && p.last_reason == 42u);
  OK(h2_pal_power_reboot(&p.api, 43u));
  assert(p.state == H2_PAL_POWER_STATE_REBOOTING &&
         p.boot_info.boot_count == 0u);
  p.capabilities.flags = 0;
  assert(h2_pal_power_deep_sleep(&p.api, 0u) == H2_PAL_ERR_UNSUPPORTED);
  h2_app_test_display_t d;
  h2_app_test_display_init(&d);
  assert(h2_pal_display_set_brightness_percent(&d.api, 10u) ==
         H2_PAL_ERR_INVALID_STATE);
  OK(h2_pal_display_open(&d.api));
  FAIL_ONCE(d.brightness);
  assert(h2_pal_display_set_brightness_percent(&d.api, 20u) == H2_PAL_ERR_IO);
  assert(d.brightness_percent == 0u && d.last_brightness_percent == 20u);
  OK(h2_pal_display_set_brightness_percent(&d.api, 30u));
  assert(d.brightness_percent == 30u);
  OK(h2_pal_display_close(&d.api));
  h2_app_test_periph_t registry;
  h2_app_test_periph_init(&registry);
  h2_app_test_periph_entry_t *e = NULL;
  h2_pal_periph_info_t i = {
      .id = 7u, .type = H2_PAL_PERIPH_TYPE_BATTERY, .name = "battery"};
  OK(h2_app_test_periph_add(&registry, &i, &e));
  e->battery = (h2_pal_battery_reading_t){
      .flags = H2_PAL_BATTERY_HAS_PERCENT_X100, .percent_x100 = 5300u};
  h2_pal_battery_reading_t b;
  OK(h2_pal_input_read_battery(&registry.input, 7u, &b));
  assert(b.id == 7u && b.percent_x100 == 5300u);
  assert(h2_app_test_periph_add(&registry, &i, &e) == H2_PAL_ERR_INVALID_STATE);
  i = (h2_pal_periph_info_t){
      .id = 8u, .type = H2_PAL_PERIPH_TYPE_PWM_SWITCH, .name = "vibration"};
  OK(h2_app_test_periph_add(&registry, &i, &e));
  FAIL_ONCE(e->write);
  assert(h2_pal_pwm_switch_set_duty(&registry.pwm, 8u, 4000u) == H2_PAL_ERR_IO);
  assert(e->duty_x100 == 0u);
  OK(h2_pal_pwm_switch_set_duty(&registry.pwm, 8u, 4000u));
  assert(e->duty_x100 == 4000u);
  assert(h2_pal_pwm_switch_set_duty(&registry.pwm, 7u, 4000u) ==
         H2_PAL_ERR_NOT_FOUND);
  i = (h2_pal_periph_info_t){
      .id = 9u, .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, .name = "button"};
  OK(h2_app_test_periph_add(&registry, &i, &e));
  e->button = H2_PAL_BUTTON_STATE_PRESSED;
  h2_pal_single_button_reading_t button;
  OK(h2_pal_button_read_single_button(&registry.button, 9u, &button));
  assert(button.state == H2_PAL_BUTTON_STATE_PRESSED);
  unsigned count = 0;
  assert(h2_pal_periph_list(&registry.api, H2_PAL_PERIPH_TYPE_ANY, periph_count,
                            &count) == H2_PAL_ERR_IO);
  assert(count == 1u);
}
static void test_pref(const h2_pal_mem_api_t *mem) {
  h2_app_test_pref_t p;
  OK(h2_app_test_pref_init(&p, mem));
  h2_pal_pref_namespace_t *w, *r, *other;
  assert(h2_pal_pref_open(&p.api, "missing", H2_PAL_PREF_OPEN_READ_ONLY, &r) ==
         H2_PAL_ERR_NOT_FOUND);
  OK(h2_pal_pref_open(&p.api, "settings", H2_PAL_PREF_OPEN_READ_WRITE, &w));
  assert(h2_pal_pref_open(&p.api, "settings", H2_PAL_PREF_OPEN_READ_WRITE,
                          &other) == H2_PAL_ERR_INVALID_STATE);
  OK(h2_pal_pref_open(&p.api, "settings", H2_PAL_PREF_OPEN_READ_ONLY, &r));
  OK(w->set_u32(w, "volume", 40u));
  uint32_t value = 0;
  assert(r->get_u32(r, "volume", &value) == H2_PAL_ERR_NOT_FOUND);
  strcpy(p.commit_namespace, "settings");
  strcpy(p.commit_key, "volume");
  FAIL_ONCE(p.commit);
  assert(w->commit(w) == H2_PAL_ERR_IO);
  assert(r->get_u32(r, "volume", &value) == H2_PAL_ERR_NOT_FOUND);
  OK(w->get_u32(w, "volume", &value));
  assert(value == 40u);
  OK(w->commit(w));
  OK(r->get_u32(r, "volume", &value));
  assert(value == 40u);
  int boolean = 0;
  assert(r->get_bool(r, "volume", &boolean) == H2_PAL_ERR_FORMAT);
  assert(r->set_bool(r, "x", 1) == H2_PAL_ERR_INVALID_STATE);
  OK(w->set_bool(w, "tutorial", 1));
  OK(w->set_i32(w, "offset", -19));
  OK(w->set_string(w, "label", "hello"));
  uint8_t bytes[] = {0u, 1u, 0u, 255u};
  OK(w->set_blob(w, "identity", bytes, sizeof(bytes)));
  OK(w->commit(w));
  char *text = NULL;
  OK(r->get_string(r, mem, "label", &text));
  assert(!strcmp(text, "hello"));
  h2_pal_mem_free(mem, text);
  void *data = NULL;
  size_t size = 0;
  OK(r->get_blob(r, mem, "identity", &data, &size));
  assert(size == sizeof(bytes) && !memcmp(data, bytes, size));
  h2_pal_mem_free(mem, data);
  int32_t offset = 0;
  OK(r->get_i32(r, "offset", &offset));
  assert(offset == -19);
  OK(r->get_bool(r, "tutorial", &boolean));
  assert(boolean == 1);
  OK(w->set_u32(w, "volume", 99u));
  OK(w->close(w));
  OK(r->get_u32(r, "volume", &value));
  assert(value == 40u);
  OK(h2_pal_pref_open(&p.api, "other", H2_PAL_PREF_OPEN_READ_WRITE, &other));
  OK(other->set_u32(other, "volume", 7u));
  OK(other->commit(other));
  OK(other->close(other));
  OK(h2_pal_pref_open(&p.api, "settings", H2_PAL_PREF_OPEN_READ_WRITE, &w));
  OK(w->remove(w, "volume"));
  FAIL_ONCE(p.commit);
  assert(w->commit(w) == H2_PAL_ERR_IO);
  OK(r->get_u32(r, "volume", &value));
  OK(w->commit(w));
  assert(r->get_u32(r, "volume", &value) == H2_PAL_ERR_NOT_FOUND);
  OK(w->clear(w));
  OK(w->commit(w));
  assert(r->get_bool(r, "tutorial", &boolean) == H2_PAL_ERR_NOT_FOUND);
  char key[30];
  for (unsigned i = 0; i < H2_APP_TEST_PREF_ENTRIES_MAX; ++i) {
    snprintf(key, sizeof(key), "key-%u", i);
    OK(w->set_u32(w, key, i));
  }
  assert(w->set_u32(w, "overflow", 0) == H2_PAL_ERR_NO_SPACE);
  assert(h2_app_test_pref_deinit(&p) == H2_PAL_ERR_INVALID_STATE);
  OK(w->close(w));
  OK(r->close(r));
  OK(h2_app_test_pref_deinit(&p));
  OK(h2_app_test_pref_deinit(&p));
}
static void test_fs(const h2_pal_mem_api_t *mem) {
  h2_app_test_fs_t f;
  OK(h2_app_test_fs_init(&f, mem));
  h2_pal_fs_file_t *h;
  size_t n = 0;
  h2_pal_fs_stat_t st;
  assert(h2_pal_fs_stat(&f.api, "/audio.pcm", &st) == H2_PAL_ERR_NOT_FOUND);
  OK(h2_pal_fs_open(&f.api, "/audio.pcm", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &h));
  OK(h2_pal_fs_write(&f.api, h, "ab", 2u, &n));
  assert(n == 2u);
  OK(h2_pal_fs_seek(&f.api, h, 4u));
  OK(h2_pal_fs_write(&f.api, h, "cd", 2u, &n));
  FAIL_ONCE(f.write);
  assert(h2_pal_fs_write(&f.api, h, "x", 1u, &n) == H2_PAL_ERR_IO && n == 0u);
  assert(h2_pal_fs_remove(&f.api, "/audio.pcm") == H2_PAL_ERR_INVALID_STATE);
  FAIL_ONCE(f.close);
  assert(h2_pal_fs_close(&f.api, h) == H2_PAL_ERR_IO);
  assert(h2_app_test_fs_deinit(&f) == H2_PAL_ERR_INVALID_STATE);
  OK(h2_pal_fs_close(&f.api, h));
  OK(h2_pal_fs_rename(&f.api, "/audio.pcm", "/fixture.pcm"));
  OK(h2_pal_fs_stat(&f.api, "/fixture.pcm", &st));
  assert(st.size == 6u);
  OK(h2_pal_fs_open(&f.api, "/fixture.pcm", H2_PAL_FS_OPEN_READ, &h));
  f.max_read_bytes = 2u;
  char data[8] = {0};
  OK(h2_pal_fs_read(&f.api, h, data, sizeof(data), &n));
  assert(n == 2u && !memcmp(data, "ab", 2u));
  f.max_read_bytes = 0u;
  OK(h2_pal_fs_read(&f.api, h, data, 8u, &n));
  assert(n == 4u && !memcmp(data, "\0\0cd", 4u));
  OK(h2_pal_fs_read(&f.api, h, data, 8u, &n));
  assert(n == 0u);
  OK(h2_pal_fs_close(&f.api, h));
  OK(h2_pal_fs_remove(&f.api, "/fixture.pcm"));
  assert(h2_pal_fs_stat(&f.api, "/fixture.pcm", &st) == H2_PAL_ERR_NOT_FOUND);
  OK(h2_pal_fs_open(&f.api, "/bound", H2_PAL_FS_OPEN_WRITE_TRUNCATE, &h));
  f.max_file_bytes = 2u;
  assert(h2_pal_fs_write(&f.api, h, "abc", 3u, &n) == H2_PAL_ERR_NO_SPACE &&
         n == 0u);
  OK(h2_pal_fs_close(&f.api, h));
  OK(h2_app_test_fs_deinit(&f));
  OK(h2_app_test_fs_deinit(&f));
}
static void test_crypto(void) {
  h2_app_test_crypto_t c;
  h2_app_test_crypto_init(&c);
  uint8_t source[] = {7, 8, 9}, out[4] = {0};
  c.random_bytes = source;
  c.random_size = 3u;
  assert(h2_pal_crypto_random(&c.api, out, 4u) == H2_PAL_ERR_NO_SPACE &&
         c.random_offset == 0u);
  OK(h2_pal_crypto_random(&c.api, out, 3u));
  assert(!memcmp(out, source, 3));
  h2_pal_x25519_keypair_t keypair;
  assert(h2_pal_crypto_x25519_keypair_generate(&c.api, &keypair) ==
         H2_PAL_ERR_UNSUPPORTED);
  c.keypair_ready = true;
  c.keypair.private_key.bytes[0] = 2;
  c.keypair.public_key.bytes[0] = 5;
  OK(h2_pal_crypto_x25519_keypair_generate(&c.api, &keypair));
  h2_pal_x25519_public_key_t public_key;
  OK(h2_pal_crypto_x25519_public_key_from_private(&c.api, &keypair.private_key,
                                                  &public_key));
  assert(public_key.bytes[0] == 5);
  keypair.private_key.bytes[0] = 3;
  assert(h2_pal_crypto_x25519_public_key_from_private(
             &c.api, &keypair.private_key, &public_key) ==
         H2_PAL_ERR_NOT_FOUND);
}
static void test_audio(const h2_pal_mem_api_t *mem) {
  h2_app_test_audio_fake_t a;
  OK(h2_app_test_audio_fake_init(&a, mem));
  h2_app_test_time_t time;
  h2_app_test_time_init(&time, 0);
  uint8_t pcm[] = {1, 2, 3, 4};
  h2_app_test_audio_fixture_t fixture = {pcm, sizeof(pcm), a.info.mic_format};
  h2_app_test_audio_t *wrapper = NULL;
  OK(h2_app_test_audio_create(mem, &time.api, &a.api, &fixture, &wrapper));
  const h2_pal_audio_api_t *api = h2_app_test_audio_api(wrapper);
  OK(h2_pal_audio_start_mic(api));
  uint8_t output[640];
  h2_audio_frame_t frame =
      h2_audio_frame_for_buffer(output, sizeof(output), a.info.mic_format);
  FAIL_ONCE(a.read_mic);
  h2_audio_frame_t malformed = frame;
  malformed.data = NULL;
  malformed.bytes = 99;
  assert(a.api.vtable->mic_read(a.api.user, &malformed, 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(malformed.bytes == 0);
  assert(a.api.vtable->mic_read(a.api.user, NULL, 0u) ==
         H2_PAL_ERR_INVALID_ARG);
  malformed = frame;
  malformed.capacity = 1;
  assert(h2_pal_audio_mic_read(&a.api, &malformed, 0u) == H2_PAL_ERR_NO_SPACE);
  assert(a.read_mic.calls == 0 && a.read_mic.remaining == 1);
  OK(h2_pal_audio_mic_read(api, &frame, 0u));
  assert(!memcmp(output, pcm, 4));
  h2_app_test_audio_evidence_t evidence;
  OK(h2_app_test_audio_copy_evidence(wrapper, &evidence));
  assert(evidence.real_capture_first_error == H2_PAL_ERR_IO);
  OK(h2_pal_audio_stop_mic(api));
  OK(h2_pal_audio_start_speaker(api));
  h2_pal_audio_track_t *track;
  h2_audio_track_config_t config = {.name = "playback",
                                    .format = a.info.playback_format};
  OK(h2_pal_audio_create_track(api, &config, &track));
  OK(h2_pal_audio_track_write(track, &frame, 10u));
  assert(a.playback_bytes == 640u);
  FAIL_ONCE(a.close_track);
  assert(h2_pal_audio_track_close(track) == H2_PAL_ERR_IO);
  assert(h2_app_test_audio_fake_deinit(&a) == H2_PAL_ERR_INVALID_STATE);
  OK(h2_pal_audio_track_close(track));
  OK(h2_pal_audio_stop_speaker(api));
  OK(h2_app_test_audio_destroy(wrapper));
  OK(h2_app_test_audio_fake_deinit(&a));
}
static void test_allocation_failures(void) {
  for (size_t offset = 1; offset <= 4; ++offset) {
    test_memory_t m = {0};
    h2_pal_mem_api_t mem = {&m, &test_memory_vtable};
    h2_app_test_pref_t p;
    OK(h2_app_test_pref_init(&p, &mem));
    m.fail_at = m.calls + offset;
    h2_pal_pref_namespace_t *h = NULL;
    int rc = h2_pal_pref_open(&p.api, "new", H2_PAL_PREF_OPEN_READ_WRITE, &h);
    if (rc == 0)
      OK(h->close(h));
    else
      assert(rc == H2_PAL_ERR_NO_MEMORY && h == NULL);
    OK(h2_app_test_pref_deinit(&p));
    assert(m.live == 0u);
  }
  test_memory_t m = {.fail_at = 1};
  h2_pal_mem_api_t mem = {&m, &test_memory_vtable};
  h2_app_test_fs_t f;
  assert(h2_app_test_fs_init(&f, &mem) == H2_PAL_ERR_NO_MEMORY);
  OK(h2_app_test_fs_deinit(&f));
  assert(m.live == 0);
}
int main(void) {
  test_memory_t m = {0};
  h2_pal_mem_api_t mem = {&m, &test_memory_vtable};
  test_time();
  test_wifi_modem();
  test_devices();
  test_pref(&mem);
  test_fs(&mem);
  test_crypto();
  test_audio(&mem);
  test_allocation_failures();
  assert(m.live == 0u);
  return 0;
}
