#include "h2_wifi_sta.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    h2_pal_wifi_sta_config_t saved, target;
    h2_pal_wifi_saved_network_t networks[H2_PAL_WIFI_SAVED_NETWORK_MAX];
    size_t network_count;
    h2_pal_wifi_sta_status_t status;
    uint64_t now;
    uint32_t connect_ms, disconnect_ms, timeout;
    unsigned int connects, disconnects, saves, reads, ready_after;
    int connect_rc, save_rc, status_rc, sleep_rc, get_rc;
    bool wrong_ssid, wrong_bssid, zero_ip;
} fixture_t;

static int get_saved(void *user, h2_pal_wifi_sta_config_t *out) {
    fixture_t *f = user;
    *out = f->saved;
    return f->get_rc ? f->get_rc : (out->ssid_len ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND);
}
static int save(void *user, const h2_pal_wifi_sta_config_t *config) {
    fixture_t *f = user;
    assert(f->status.state == H2_PAL_WIFI_STA_STATE_GOT_IP);
    assert(f->status.ip_valid && f->status.ip.ip4);
    assert(!f->wrong_ssid && !f->wrong_bssid);
    ++f->saves;
    if (!f->save_rc) {
      if (!f->network_count && f->saved.ssid_len)
        assert(h2_wifi_saved_list_insert(f->networks, &f->network_count,
                                         &f->saved) == H2_PAL_OK);
      assert(h2_wifi_saved_list_insert(f->networks, &f->network_count,
                                       config) == H2_PAL_OK);
      f->saved = f->networks[0].config;
    }
    return f->save_rc;
}
static int connect(void *user, const h2_pal_wifi_sta_config_t *config, uint32_t timeout) {
    fixture_t *f = user;
    ++f->connects;
    f->target = *config;
    f->timeout = timeout;
    f->now += f->connect_ms;
    f->reads = 0;
    f->status = (h2_pal_wifi_sta_status_t){.state = H2_PAL_WIFI_STA_STATE_CONNECTING};
    return f->connect_rc;
}
static int disconnect(void *user) {
    fixture_t *f = user;
    ++f->disconnects;
    f->now += f->disconnect_ms;
    f->status = (h2_pal_wifi_sta_status_t){.state = H2_PAL_WIFI_STA_STATE_IDLE};
    return H2_PAL_OK;
}
static int status(void *user, h2_pal_wifi_sta_status_t *out) {
    fixture_t *f = user;
    if (f->connects && ++f->reads > f->ready_after) {
        f->status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
        f->status.ip_valid = 1;
        f->status.ip.ip4 = f->zero_ip ? 0 : 0xc0000201;
        f->status.ssid_len = f->target.ssid_len;
        memcpy(f->status.ssid, f->target.ssid, f->target.ssid_len);
        if (f->wrong_ssid) f->status.ssid[0] ^= 1;
        f->status.bssid_set = f->target.bssid_set;
        memcpy(f->status.bssid, f->target.bssid, 6);
        if (f->wrong_bssid) f->status.bssid[0] ^= 1;
    }
    *out = f->status;
    return f->status_rc;
}
static h2_pal_result_t now(void *user, uint64_t *out) {
    *out = ((fixture_t *)user)->now;
    return H2_PAL_OK;
}
static h2_pal_result_t sleep_ms(void *user, uint32_t ms) {
    fixture_t *f = user;
    f->now += ms;
    return f->sleep_rc;
}
static const h2_pal_wifi_sta_vtable_t sta_vtable = {
    .connect = connect, .disconnect = disconnect, .get_status = status};
static const h2_pal_wifi_settings_vtable_t settings_vtable = {
    .get_saved_sta_config = get_saved, .set_saved_sta_config = save};
static const h2_pal_time_vtable_t time_vtable = {
    .get_monotonic_ms = now, .sleep_ms = sleep_ms};

static void run_cases(void) {
    fixture_t f = {0};
    const h2_pal_wifi_sta_api_t sta = {&f, &sta_vtable};
    const h2_pal_wifi_settings_api_t settings = {&f, &settings_vtable};
    const h2_pal_time_api_t time = {&f, &time_vtable};
    h2_wifi_sta_dependencies_t deps = {&sta, &settings, &time};
    const h2_pal_wifi_sta_config_t old = {.ssid = "home", .ssid_len = 4,
        .password = "old-password", .password_len = 12};
    h2_pal_wifi_sta_config_t target = {.ssid = "temporary", .ssid_len = 9,
        .password = "new-password", .password_len = 12};
    const uint32_t budgets[] = {0, 1, 100, UINT32_MAX};
    for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
        f = (fixture_t){0};
        assert(h2_pal_wifi_sta_connect(&sta, &target, budgets[i]) == H2_PAL_OK);
        assert(!f.saves && !f.saved.ssid_len);
        f.saved = old;
        assert(h2_pal_wifi_sta_connect(&sta, &target, budgets[i]) == H2_PAL_OK);
        assert(!f.saves && !memcmp(&f.saved, &old, sizeof(old)));
    }
    f = (fixture_t){.saved = old, .ready_after = 2};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_OK);
    assert(f.saves == 1 && f.reads == 3 && f.now == 40);
    assert(f.network_count == 2u);
    assert(!memcmp(&f.networks[1].config, &old, sizeof(old)));
    assert(!memcmp(&f.saved, &target, sizeof(target)));
    f = (fixture_t){.saved = old, .connect_rc = H2_PAL_ERR_IO};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_IO);
    assert(!f.saves && !memcmp(&f.saved, &old, sizeof(old)));
    f = (fixture_t){.saved = old, .save_rc = H2_PAL_ERR_IO};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_IO);
    assert(f.saves == 1 && !memcmp(&f.saved, &old, sizeof(old)));
    for (unsigned int i = 0; i < 4; ++i) {
        f = (fixture_t){.saved = old, .wrong_ssid = i == 0,
            .wrong_bssid = i == 1, .zero_ip = i == 2,
            .ready_after = i == 3 ? 100 : 0};
        target.bssid_set = 1;
        assert(h2_wifi_sta_connect_and_save(&deps, &target, 31) == H2_PAL_ERR_TIMEOUT);
        assert(f.now == 31 && !f.saves && !memcmp(&f.saved, &old, sizeof(old)));
    }
    f = (fixture_t){.saved = old};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 0) == H2_PAL_ERR_INVALID_ARG);
    assert(!f.connects && !f.disconnects && !f.saves);
    assert(h2_pal_wifi_sta_connect_and_save(&sta, &target, 100) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_wifi_sta_connect_and_save(NULL, &target, 100) == H2_PAL_ERR_INVALID_ARG);
    target.ssid_len = H2_PAL_WIFI_SSID_MAX + 1;
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_INVALID_ARG);
    target = old;
    target.password[0] = 'x';
    f = (fixture_t){.saved = old, .status.state = H2_PAL_WIFI_STA_STATE_GOT_IP,
        .disconnect_ms = 30, .connect_rc = H2_PAL_ERR_IO};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_IO);
    assert(f.disconnects == 1 && f.timeout == 70 && !memcmp(&f.saved, &old, sizeof(old)));
    f = (fixture_t){.saved = old, .status.state = H2_PAL_WIFI_STA_STATE_CONNECTED,
        .disconnect_ms = 100};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_TIMEOUT);
    assert(!f.connects && !f.saves);
    f = (fixture_t){.saved = old, .connect_ms = 100};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_TIMEOUT);
    assert(!f.saves);
    f = (fixture_t){.saved = old, .get_rc = H2_PAL_ERR_UNSUPPORTED};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_UNSUPPORTED);
    assert(!f.connects && !f.disconnects && !f.saves);
    f = (fixture_t){.saved = old, .status_rc = H2_PAL_ERR_IO};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_IO);
    assert(!f.connects && !f.saves);
    f = (fixture_t){.saved = old, .sleep_rc = H2_PAL_ERR_IO, .ready_after = 100};
    assert(h2_wifi_sta_connect_and_save(&deps, &target, 100) == H2_PAL_ERR_IO);
    assert(!f.saves);
}
static int clear_legacy(void *user) {
  fixture_t *f = user;
  memset(&f->saved, 0, sizeof(f->saved));
  return H2_PAL_OK;
}

static void test_fallback(void) {
  fixture_t f = {0};
  const h2_pal_wifi_settings_vtable_t vtable = {
      .get_saved_sta_config = get_saved,
      .clear_saved_sta_config = clear_legacy};
  const h2_pal_wifi_settings_api_t settings = {&f, &vtable};
  h2_pal_wifi_saved_network_t out[8] = {0};
  size_t count = 99;
  assert(h2_pal_wifi_settings_list_saved_sta_configs(&settings, out, 8,
                                                     &count) == H2_PAL_OK);
  assert(count == 0);
  f.saved = (h2_pal_wifi_sta_config_t){.ssid = "home", .ssid_len = 4};
  assert(h2_pal_wifi_settings_list_saved_sta_configs(&settings, out, 8,
                                                     &count) == H2_PAL_OK);
  assert(count == 1 && out[0].config.ssid_len == 4 &&
         out[0].last_connected_seq == 0);
  assert(h2_pal_wifi_settings_list_saved_sta_configs(&settings, NULL, 0,
                                                     &count) == H2_PAL_OK);
  assert(count == 0);
  assert(h2_pal_wifi_settings_list_saved_sta_configs(
             &settings, NULL, 1, &count) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_wifi_settings_list_saved_sta_configs(NULL, out, 8, &count) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_wifi_settings_list_saved_sta_configs(&settings, out, 8, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_wifi_settings_remove_saved_sta_config(&settings, "HOME", 4) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(f.saved.ssid_len == 4);
  f.get_rc = H2_PAL_ERR_IO;
  assert(h2_pal_wifi_settings_list_saved_sta_configs(&settings, out, 8,
                                                     &count) == H2_PAL_ERR_IO);
  assert(h2_pal_wifi_settings_remove_saved_sta_config(&settings, "home", 4) ==
         H2_PAL_ERR_IO);
  f.get_rc = 0;
  assert(h2_pal_wifi_settings_remove_saved_sta_config(&settings, "home", 4) ==
         H2_PAL_OK);
  assert(!f.saved.ssid_len);
  assert(h2_pal_wifi_settings_remove_saved_sta_config(&settings, "home", 4) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_wifi_settings_remove_saved_sta_config(&settings, NULL, 4) ==
         H2_PAL_ERR_INVALID_ARG);
}

static void test_saved_list(void) {
  h2_pal_wifi_saved_network_t saved[8] = {0}, decoded[8] = {0};
  size_t count = 0, decoded_count = 0;
  uint8_t blob[H2_WIFI_SAVED_LIST_BLOB_SIZE];
  assert(sizeof(blob) == 888u);
  assert(h2_wifi_saved_list_encode(NULL, 0, blob, sizeof(blob)) == H2_PAL_OK);
  assert(h2_wifi_saved_list_decode(blob, sizeof(blob), decoded,
                                   &decoded_count) == H2_PAL_OK);
  assert(decoded_count == 0);
  for (unsigned int i = 0; i < 9; ++i) {
    h2_pal_wifi_sta_config_t c = {.ssid = "net0", .ssid_len = 4};
    c.ssid[3] = (char)('0' + i);
    assert(h2_wifi_saved_list_insert(saved, &count, &c) == H2_PAL_OK);
    assert(count == (i < 8 ? i + 1 : 8));
  }
  assert(saved[0].config.ssid[3] == '8' && saved[7].config.ssid[3] == '1');
  h2_pal_wifi_sta_config_t updated = saved[4].config;
  memcpy(updated.password, "new", 3);
  updated.password_len = 3;
  assert(h2_wifi_saved_list_insert(saved, &count, &updated) == H2_PAL_OK);
  assert(count == 8 && saved[0].config.ssid[3] == '4');
  assert(saved[0].config.password_len == 3 && saved[1].config.ssid[3] == '8');
  saved[0].last_connected_seq = UINT32_MAX;
  assert(h2_wifi_saved_list_insert(saved, &count, &saved[2].config) ==
         H2_PAL_OK);
  assert(saved[0].last_connected_seq == 9 && saved[1].last_connected_seq == 8);
  assert(h2_wifi_saved_list_remove(saved, &count, "net8", 4) == H2_PAL_OK);
  assert(count == 7 && saved[0].config.ssid[3] == '7' &&
         saved[1].config.ssid[3] == '4');
  assert(saved[2].config.ssid[3] == '6' && saved[7].config.ssid_len == 0);
  assert(h2_wifi_saved_list_remove(saved, &count, "net8", 4) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(h2_wifi_saved_list_encode(saved, count, blob, sizeof(blob)) ==
         H2_PAL_OK);
  assert(!memcmp(blob, "H2WN\1\0\7\0", 8));
  assert(blob[8] == 4 && blob[8 + 106] == 9);
  assert(h2_wifi_saved_list_decode(blob, sizeof(blob), decoded,
                                   &decoded_count) == H2_PAL_OK);
  assert(decoded_count == count && !memcmp(saved, decoded, sizeof(saved)));
  const size_t bad_offsets[] = {0, 4, 5, 6, 7, 8, 9, 16, 17};
  for (size_t i = 0; i < sizeof(bad_offsets) / sizeof(bad_offsets[0]); ++i) {
    uint8_t old = blob[bad_offsets[i]];
    blob[bad_offsets[i]] = 255;
    assert(h2_wifi_saved_list_decode(blob, sizeof(blob), decoded,
                                     &decoded_count) == H2_PAL_ERR_FORMAT);
    assert(decoded_count == 0);
    blob[bad_offsets[i]] = old;
  }
  assert(h2_wifi_saved_list_decode(blob, sizeof(blob) - 1, decoded,
                                   &decoded_count) == H2_PAL_ERR_FORMAT);
  memcpy(blob + 8 + H2_WIFI_SAVED_LIST_RECORD_SIZE, blob + 8,
         H2_WIFI_SAVED_LIST_RECORD_SIZE);
  assert(h2_wifi_saved_list_decode(blob, sizeof(blob), decoded,
                                   &decoded_count) == H2_PAL_ERR_FORMAT);
  updated.ssid_len = 33;
  assert(h2_wifi_saved_list_insert(saved, &count, &updated) ==
         H2_PAL_ERR_INVALID_ARG);
  count = 9;
  assert(h2_wifi_saved_list_encode(saved, count, blob, sizeof(blob)) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_wifi_saved_list_remove(saved, &count, "net7", 4) ==
         H2_PAL_ERR_INVALID_ARG);
  /* Maximum-length byte strings do not require a terminator in the blob. */
  count = 0;
  updated = (h2_pal_wifi_sta_config_t){.ssid_len = 32,
                                       .password_len = 64,
                                       .bssid = {1, 2, 3, 4, 5, 6},
                                       .bssid_set = 1,
                                       .channel = 149};
  memset(updated.ssid, 's', 32);
  memset(updated.password, 'p', 64);
  assert(h2_wifi_saved_list_insert(saved, &count, &updated) == H2_PAL_OK);
  assert(h2_wifi_saved_list_encode(saved, count, blob, sizeof(blob)) ==
         H2_PAL_OK);
  assert(h2_wifi_saved_list_decode(blob, sizeof(blob), decoded,
                                   &decoded_count) == H2_PAL_OK);
  assert(decoded_count == 1 &&
         !memcmp(&decoded[0].config, &updated, sizeof(updated)));
}

static void test_ranking(void) {
  const h2_pal_wifi_saved_network_t saved[] = {
      {.config = {.ssid = "a",
                  .ssid_len = 1,
                  .password = "secret",
                  .password_len = 6},
       .last_connected_seq = 2},
      {.config = {.ssid = "b", .ssid_len = 1}, .last_connected_seq = 1},
      {.config = {.ssid = "hidden", .ssid_len = 6}, .last_connected_seq = 3}};
  h2_pal_wifi_scan_entry_t scan[] = {
      {.ssid = "a", .ssid_len = 1, .rssi = -70, .channel = 1, .bssid = {1}},
      {.ssid = "b", .ssid_len = 1, .rssi = -30, .channel = 6, .bssid = {2}},
      {.ssid = "a", .ssid_len = 1, .rssi = -40, .channel = 11, .bssid = {3}},
      {.ssid = "unknown", .ssid_len = 7, .rssi = -10}};
  h2_pal_wifi_sta_config_t out[8];
  size_t count;
  assert(h2_wifi_sta_rank_saved_candidates(saved, 3, scan, 4, out, &count) ==
         H2_PAL_OK);
  assert(count == 2 && out[0].ssid[0] == 'b' && out[1].ssid[0] == 'a');
  assert(out[0].bssid_set && out[0].bssid[0] == 2 && out[0].channel == 6);
  assert(out[1].bssid[0] == 3 && out[1].channel == 11 &&
         out[1].password_len == 6);
  scan[2].rssi = -30;
  assert(h2_wifi_sta_rank_saved_candidates(saved, 3, scan, 4, out, &count) ==
         H2_PAL_OK);
  assert(count == 2 && out[0].ssid[0] == 'a');
  assert(h2_wifi_sta_rank_saved_candidates(saved, 3, NULL, 0, out, &count) ==
         H2_PAL_OK);
  assert(count == 0);
  scan[0].ssid_len = 33;
  assert(h2_wifi_sta_rank_saved_candidates(saved, 3, scan, 4, out, &count) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_wifi_sta_rank_saved_candidates(NULL, 1, NULL, 0, out, &count) ==
         H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
    run_cases();
    test_fallback();
    test_saved_list();
    test_ranking();
    puts("Wi-Fi transaction tests passed");
    return 0;
}
