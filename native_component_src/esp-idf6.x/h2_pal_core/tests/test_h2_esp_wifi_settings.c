#include "h2_esp_platform_core.h"
#include "h2_esp_wifi_saved_record.h"

#include <assert.h>
#include <string.h>

#ifdef H2_ESP_WIFI_SETTINGS_UNSUPPORTED_TEST
int main(void) {
  h2_pal_wifi_settings_t *settings = h2_esp_platform_wifi_settings();
  h2_pal_wifi_saved_network_t out;
  size_t count = 42;
  assert(h2_pal_wifi_settings_list_saved_sta_configs(
             settings, &out, 1, &count) == H2_PAL_ERR_UNSUPPORTED);
  assert(count == 0);
  assert(h2_pal_wifi_settings_remove_saved_sta_config(settings, "A", 1) ==
         H2_PAL_ERR_UNSUPPORTED);
  return 0;
}
#else
static uint8_t saved[H2_WIFI_SAVED_LIST_BLOB_SIZE];
static uint8_t legacy[H2_ESP_WIFI_SAVED_RECORD_SIZE] = {1u};
static bool present;
static bool locked;
static int start_result = H2_PAL_OK;
static int read_result = H2_PAL_OK;
static int write_result = H2_PAL_OK;
static unsigned writes;
static unsigned imports;

int h2_esp_platform_wifi_ensure_started(void) { return start_result; }

int h2_esp_platform_wifi_saved_transaction(int (*operation)(void *),
                                           void *context) {
  assert(!locked);
  locked = true;
  int rc = operation(context);
  locked = false;
  return rc;
}

int h2_esp_platform_wifi_saved_record(uint8_t *record, bool write) {
  assert(locked);
  if (!write && read_result != H2_PAL_OK)
    return read_result;
  if (!write && !present) {
    int rc = h2_esp_wifi_saved_import(legacy, record);
    if (rc != H2_PAL_OK)
      return rc;
    if (write_result != H2_PAL_OK)
      return write_result;
    memcpy(saved, record, sizeof(saved));
    present = true;
    ++imports;
  }
  if (write) {
    if (write_result != H2_PAL_OK)
      return write_result;
    memcpy(saved, record, sizeof(saved));
    present = true;
    ++writes;
  } else
    memcpy(record, saved, sizeof(saved));
  return H2_PAL_OK;
}

static h2_pal_wifi_sta_config_t config(char id) {
  h2_pal_wifi_sta_config_t result = {.ssid_len = 1, .password_len = 8};
  result.ssid[0] = id;
  memcpy(result.password, "password", 8);
  return result;
}

static void expect_order(h2_pal_wifi_settings_t *settings, const char *order) {
  h2_pal_wifi_saved_network_t out[H2_PAL_WIFI_SAVED_NETWORK_MAX];
  size_t count = 99;
  assert(h2_pal_wifi_settings_list_saved_sta_configs(
             settings, out, H2_PAL_WIFI_SAVED_NETWORK_MAX, &count) ==
         H2_PAL_OK);
  assert(count == strlen(order));
  for (size_t i = 0; i < count; ++i) {
    assert(out[i].config.ssid_len == 1);
    assert(out[i].config.ssid[0] == order[i]);
    if (i)
      assert(out[i - 1].last_connected_seq > out[i].last_connected_seq);
  }
  h2_pal_wifi_sta_config_t recent;
  memset(&recent, 0xff, sizeof(recent));
  int rc = h2_pal_wifi_settings_get_saved_sta_config(settings, &recent);
  if (count) {
    assert(rc == H2_PAL_OK && recent.ssid[0] == order[0]);
  } else {
    const h2_pal_wifi_sta_config_t empty = {0};
    assert(rc == H2_PAL_ERR_NOT_FOUND);
    assert(memcmp(&recent, &empty, sizeof(empty)) == 0);
  }
  int has = -1;
  assert(h2_pal_wifi_settings_has_saved_sta_config(settings, &has) ==
         H2_PAL_OK);
  assert(has == (count != 0));
}

int main(void) {
  h2_pal_wifi_settings_t *settings = h2_esp_platform_wifi_settings();
  expect_order(settings, "");
  assert(imports == 1);
  h2_pal_wifi_sta_config_t a = config('A'), b = config('B');
  assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &a) == H2_PAL_OK);
  assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &b) == H2_PAL_OK);
  expect_order(settings, "BA");
  memcpy(a.password, "new-pass", 8);
  a.bssid_set = 1;
  memcpy(a.bssid, "123456", 6);
  a.channel = 11;
  assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &a) == H2_PAL_OK);
  expect_order(settings, "AB");
  h2_pal_wifi_sta_config_t out;
  assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) ==
         H2_PAL_OK);
  assert(memcmp(&out, &a, sizeof(a)) == 0);
  for (char id = 'C'; id <= 'I'; ++id) {
    h2_pal_wifi_sta_config_t next = config(id);
    assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &next) ==
           H2_PAL_OK);
  }
  expect_order(settings, "IHGFEDCA");
  assert(h2_pal_wifi_settings_remove_saved_sta_config(settings, "F", 1) ==
         H2_PAL_OK);
  expect_order(settings, "IHGEDCA");
  assert(h2_pal_wifi_settings_remove_saved_sta_config(settings, "B", 1) ==
         H2_PAL_ERR_NOT_FOUND);
  expect_order(settings, "IHGEDCA");
  size_t count = 99;
  assert(h2_pal_wifi_settings_list_saved_sta_configs(settings, NULL, 0,
                                                     &count) == H2_PAL_OK);
  assert(count == 0);
  h2_pal_wifi_saved_network_t first;
  assert(h2_pal_wifi_settings_list_saved_sta_configs(settings, &first, 1,
                                                     &count) == H2_PAL_OK);
  assert(count == 1 && first.config.ssid[0] == 'I');

  /* Read and write failures cannot publish a partial or modified list. */
  uint8_t before[sizeof(saved)];
  memcpy(before, saved, sizeof(saved));
  unsigned previous_writes = writes;
  write_result = H2_PAL_ERR_IO;
  assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &b) ==
         H2_PAL_ERR_IO);
  assert(h2_pal_wifi_settings_remove_saved_sta_config(settings, "I", 1) ==
         H2_PAL_ERR_IO);
  assert(h2_pal_wifi_settings_clear_saved_sta_config(settings) ==
         H2_PAL_ERR_IO);
  assert(writes == previous_writes &&
         memcmp(saved, before, sizeof(saved)) == 0);
  write_result = H2_PAL_OK;
  read_result = H2_PAL_ERR_IO;
  assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &b) ==
         H2_PAL_ERR_IO);
  memset(&first, 0xa5, sizeof(first));
  h2_pal_wifi_saved_network_t untouched = first;
  assert(h2_pal_wifi_settings_list_saved_sta_configs(settings, &first, 1,
                                                     &count) == H2_PAL_ERR_IO);
  assert(count == 0 && memcmp(&first, &untouched, sizeof(first)) == 0);
  read_result = H2_PAL_OK;
  expect_order(settings, "IHGEDCA");

  const unsigned offsets[] = {0, 4, 6, 7, 8, 9, 16, 17};
  for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
    saved[offsets[i]] = 255;
    assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) ==
           H2_PAL_ERR_IO);
    memcpy(saved, before, sizeof(saved));
  }
  const uint8_t invalid[] = {15, 35, 37, 65, 99, 145, 148, 178, 255};
  for (size_t i = 0; i < sizeof(invalid); ++i) {
    b.channel = invalid[i];
    assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &b) ==
           H2_PAL_ERR_INVALID_ARG);
  }
  b.channel = 0;
  assert(h2_pal_wifi_settings_clear_saved_sta_config(settings) == H2_PAL_OK);
  expect_order(settings, "");
  assert(saved[6] == 0 && imports == 1);

  /* Import the actual 107-byte V1 layout, preserving full credentials. */
  memset(legacy, 0, sizeof(legacy));
  legacy[0] = 1;
  legacy[1] = 1;
  legacy[2] = 8;
  legacy[3] = 1;
  legacy[4] = 11;
  memcpy(legacy + 5, "123456", 6);
  memcpy(legacy + 11, "A", 1);
  memcpy(legacy + 43, "new-pass", 8);
  present = false;
  write_result = H2_PAL_ERR_IO;
  assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) ==
         H2_PAL_ERR_IO);
  assert(!present);
  write_result = H2_PAL_OK;
  expect_order(settings, "A");
  assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) ==
         H2_PAL_OK);
  assert(memcmp(&out, &a, sizeof(a)) == 0);
  assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &b) == H2_PAL_OK);
  expect_order(settings, "BA");
  legacy[11] = 'X';
  expect_order(settings, "BA");
  assert(imports == 2);
  assert(h2_pal_wifi_settings_clear_saved_sta_config(settings) == H2_PAL_OK);
  expect_order(settings, "");
  assert(imports == 2);
  /* Migrated legacy tombstones remain empty. */
  present = false;
  legacy[1] = 0;
  expect_order(settings, "");
  h2_pal_wifi_sta_config_t maximum = {0};
  memset(maximum.ssid, 's', H2_PAL_WIFI_SSID_MAX);
  memset(maximum.password, 'p', H2_PAL_WIFI_PASSWORD_MAX);
  maximum.ssid_len = H2_PAL_WIFI_SSID_MAX;
  maximum.password_len = H2_PAL_WIFI_PASSWORD_MAX;
  const uint8_t valid_channels[] = {0, 14, 36, 64, 100, 144, 149, 177};
  for (size_t i = 0; i < sizeof(valid_channels); ++i) {
    maximum.channel = valid_channels[i];
    assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &maximum) ==
           H2_PAL_OK);
    assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) ==
           H2_PAL_OK);
    assert(memcmp(&out, &maximum, sizeof(out)) == 0);
  }
  /* Malformed legacy lengths must never reach an unchecked memcpy. */
  uint8_t imported[H2_WIFI_SAVED_LIST_BLOB_SIZE];
  const unsigned invalid_offsets[] = {0, 1, 2, 3, 4};
  legacy[1] = 1;
  for (size_t i = 0; i < sizeof(invalid_offsets) / sizeof(invalid_offsets[0]);
       ++i) {
    uint8_t old = legacy[invalid_offsets[i]];
    legacy[invalid_offsets[i]] = 255;
    assert(h2_esp_wifi_saved_import(legacy, imported) == H2_PAL_ERR_IO);
    legacy[invalid_offsets[i]] = old;
  }
  start_result = H2_PAL_ERR_UNAVAILABLE;
  assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) ==
         H2_PAL_ERR_UNAVAILABLE);
  return 0;
}

#endif
