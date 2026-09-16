#include "h2_esp_platform_core.h"
#include "h2_esp_wifi_saved_record.h"

#include <assert.h>
#include <string.h>

static uint8_t saved[H2_ESP_WIFI_SAVED_RECORD_SIZE] = {1u};
static int start_result = H2_PAL_OK;
static int storage_result = H2_PAL_OK;
static unsigned writes;

int h2_esp_platform_wifi_ensure_started(void) {
    return start_result;
}

int h2_esp_platform_wifi_saved_record(uint8_t *record, bool write) {
    if (storage_result != H2_PAL_OK) {
        return storage_result;
    }
    if (write) {
        memcpy(saved, record, sizeof(saved));
        ++writes;
    } else {
        memcpy(record, saved, sizeof(saved));
    }
    return H2_PAL_OK;
}

static void assert_empty(const h2_pal_wifi_sta_config_t *config) {
    const h2_pal_wifi_sta_config_t empty = {0};
    assert(memcmp(config, &empty, sizeof(empty)) == 0);
}

int main(void) {
    h2_pal_wifi_settings_t *settings = h2_esp_platform_wifi_settings();
    h2_pal_wifi_sta_config_t out;
    int has = 1;
    memset(&out, 0xff, sizeof(out));
    assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) == H2_PAL_ERR_NOT_FOUND);
    assert_empty(&out);
    assert(h2_pal_wifi_settings_has_saved_sta_config(settings, &has) == H2_PAL_OK && !has);

    h2_pal_wifi_sta_config_t input = {0};
    memset(input.ssid, 's', H2_PAL_WIFI_SSID_MAX);
    memset(input.password, 'p', H2_PAL_WIFI_PASSWORD_MAX);
    input.ssid_len = H2_PAL_WIFI_SSID_MAX;
    input.password_len = H2_PAL_WIFI_PASSWORD_MAX;
    input.channel = 11u;
    input.bssid_set = 1u;
    memcpy(input.bssid, "123456", 6u);
    assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &input) == H2_PAL_OK);
    assert(writes == 1u);
    assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) == H2_PAL_OK);
    assert(memcmp(&input, &out, sizeof(input)) == 0);
    assert(h2_pal_wifi_settings_has_saved_sta_config(settings, &has) == H2_PAL_OK && has);

    /* A failed persistence operation must preserve the previous saved network. */
    storage_result = H2_PAL_ERR_IO;
    assert(h2_pal_wifi_settings_clear_saved_sta_config(settings) == H2_PAL_ERR_IO);
    assert(writes == 1u);
    memset(&out, 0xff, sizeof(out));
    assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) == H2_PAL_ERR_IO);
    assert_empty(&out);
    storage_result = H2_PAL_OK;
    assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) == H2_PAL_OK);
    assert(memcmp(&input, &out, sizeof(input)) == 0);

    /* Reject malformed storage before using its lengths for copies. */
    const unsigned offsets[] = {0u, 1u, 2u, 3u, 4u};
    for (unsigned i = 0u; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        const uint8_t original = saved[offsets[i]];
        saved[offsets[i]] = 255u;
        memset(&out, 0xff, sizeof(out));
        assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) == H2_PAL_ERR_IO);
        assert_empty(&out);
        saved[offsets[i]] = original;
    }

    const uint8_t invalid_channels[] = {15u, 35u, 37u, 65u, 99u, 145u, 148u, 178u, 255u};
    for (unsigned i = 0u; i < sizeof(invalid_channels); ++i) {
        saved[4] = invalid_channels[i];
        assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) == H2_PAL_ERR_IO);
        assert_empty(&out);
        input.channel = invalid_channels[i];
        assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &input) == H2_PAL_ERR_INVALID_ARG);
    }
    const uint8_t valid_channels[] = {0u, 14u, 36u, 64u, 100u, 144u, 149u, 177u};
    for (unsigned i = 0u; i < sizeof(valid_channels); ++i) {
        input.channel = valid_channels[i];
        assert(h2_pal_wifi_settings_set_saved_sta_config(settings, &input) == H2_PAL_OK);
        assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) == H2_PAL_OK);
        assert(out.channel == input.channel);
    }

    assert(h2_pal_wifi_settings_clear_saved_sta_config(settings) == H2_PAL_OK);
    assert(saved[0] == 1u && saved[1] == 0u);
    for (unsigned i = 1u; i < sizeof(saved); ++i) {
        assert(saved[i] == 0u);
    }
    assert(h2_pal_wifi_settings_has_saved_sta_config(settings, &has) == H2_PAL_OK && !has);
    start_result = H2_PAL_ERR_UNAVAILABLE;
    memset(&out, 0xff, sizeof(out));
    assert(h2_pal_wifi_settings_get_saved_sta_config(settings, &out) == H2_PAL_ERR_UNAVAILABLE);
    assert_empty(&out);
    return 0;
}
