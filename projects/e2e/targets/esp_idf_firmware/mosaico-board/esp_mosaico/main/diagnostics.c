#include "diagnostics.h"
#include "h2_pal_audio_e2e.h"
#include "bsp/magnetometer.h"
#include "bsp/esp_mosaico.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

mosaico_diagnostics_t mosaico_diagnostics = {
    .camera = "CAM: PENDING", .battery = "BAT TEST: PENDING", .audio = "AUDIO: PENDING",
    .wifi = "WIFI: PENDING", .ble = "BLE: PENDING", .stage = "PREPARING TESTS"
};
static void (*refresh_screen)(void *);
static void *refresh_user;
static atomic_uint ble_reports;
static void step(const char *name) {
    snprintf(mosaico_diagnostics.stage, sizeof(mosaico_diagnostics.stage), "%s", name);
    printf("H2_MOSAICO_STAGE %s\n", name);
    fflush(stdout);
    if (refresh_screen) refresh_screen(refresh_user);
}
static void record(const char *name, int rc) {
    printf("H2_MOSAICO_EXT name=%s rc=%d status=%s\n", name, rc,
           rc == 0 ? "PASS" : rc == H2_PAL_ERR_UNSUPPORTED ? "BLOCKED" : "FAIL");
    fflush(stdout);
}
static void raw_magnetometer(int id, const char *phase) {
    struct bmm150_dev *dev = bsp_magnetometer_get_handle(id);
    if (!dev) return;
    uint8_t data[8] = {0}, status = 0;
    int rc = bmm150_get_regs(0x42, data, sizeof(data), dev);
    const int status_rc = bmm150_get_regs(0x4a, &status, 1, dev);
    int x = ((unsigned)data[1] << 5) | (data[0] >> 3);
    int y = ((unsigned)data[3] << 5) | (data[2] >> 3);
    int z = ((unsigned)data[5] << 7) | (data[4] >> 1);
    if (x >= 4096) x -= 8192;
    if (y >= 4096) y -= 8192;
    if (z >= 16384) z -= 32768;
    unsigned rhall = ((unsigned)data[7] << 6) | (data[6] >> 2);
    printf("H2_MOSAICO_MAG_RAW id=%d phase=%s rc=%d status_rc=%d xyz=%d,%d,%d rhall=%u overflow=%u trim_xyz1=%u trim_z1=%u trim_z2=%d\n",
           id, phase, rc, status_rc, x, y, z, rhall, (status >> 6) & 1,
           dev->trim_data.dig_xyz1, dev->trim_data.dig_z1, dev->trim_data.dig_z2);
    uint8_t config[8] = {0};
    int config_rc = bmm150_get_regs(0x4b, config, sizeof(config), dev);
    printf("H2_MOSAICO_MAG_CONFIG id=%d rc=%d power=%02x mode=%02x int=%02x axes=%02x repxy=%u repz=%u\n",
           id, config_rc, config[0], config[1], config[2], config[3], config[6], config[7]);
    if (rc == 0 && (x == -4096 || y == -4096 || z == -16384))
        printf("H2_MOSAICO_MAG_DIAG id=%d reason=raw_sensor_overflow\n", id);
    else if (rc == 0 && rhall == 0)
        printf("H2_MOSAICO_MAG_DIAG id=%d reason=invalid_hall_sample\n", id);
}
static void magnetometers(void) {
    step("MAG RAW / SELF TEST / RESET");
    for (int id = 0; id < 2; ++id) {
        int rc = bsp_magnetometer_init(id);
        if (rc != 0) { record("mag_init", rc); continue; }
        vTaskDelay(pdMS_TO_TICKS(150));
        raw_magnetometer(id, "before");
        struct bmm150_dev *dev = bsp_magnetometer_get_handle(id);
        const int self = bmm150_perform_self_test(BMM150_SELF_TEST_NORMAL, dev);
        printf("H2_MOSAICO_MAG_SELF id=%d rc=%d\n", id, self);
        rc = bmm150_soft_reset(dev);
        if (rc == 0) rc = bmm150_init(dev);
        struct bmm150_settings settings = {
            .pwr_mode = BMM150_POWERMODE_NORMAL, .preset_mode = BMM150_PRESETMODE_REGULAR
        };
        if (rc == 0) rc = bmm150_set_op_mode(&settings, dev);
        if (rc == 0) rc = bmm150_set_presetmode(&settings, dev);
        printf("H2_MOSAICO_MAG_RESTORE id=%d rc=%d\n", id, rc);
        vTaskDelay(pdMS_TO_TICKS(150));
        if (rc == 0) raw_magnetometer(id, "after_reset");
    }
}
static void battery(h2_runtime_t *runtime) {
    step("BAT READ ONLY / 8 SAMPLES");
    int first = 0;
    for (int i = 0; i < 8; ++i) {
        h2_pal_battery_reading_t b = {0};
        int rc = h2_pal_input_read_battery(runtime->input, 301, &b);
        const uint32_t required = H2_PAL_BATTERY_HAS_VOLTAGE_MV |
            H2_PAL_BATTERY_HAS_CURRENT_MA | H2_PAL_BATTERY_HAS_PERCENT_X100;
        if (rc == 0 && ((b.flags & required) != required || b.id != 301 ||
            b.voltage_mv < 2500 || b.voltage_mv > 4500 || b.percent_x100 > 10000))
            rc = H2_PAL_ERR_INVALID_STATE;
        printf("H2_MOSAICO_BAT_SAMPLE n=%d rc=%d mv=%" PRId32 " ma=%" PRId32 " soc=%u\n",
               i, rc, b.voltage_mv, b.current_ma, b.percent_x100);
        if (first == 0) first = rc;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    h2_pal_battery_reading_t b = {0};
    if (h2_pal_input_read_battery(runtime->input, 0, &b) != H2_PAL_ERR_NOT_FOUND && first == 0)
        first = H2_PAL_ERR_INVALID_STATE;
    if (h2_pal_input_read_battery(runtime->input, 301, NULL) != H2_PAL_ERR_INVALID_ARG && first == 0)
        first = H2_PAL_ERR_INVALID_STATE;
    record("battery_read_contract", first);
    snprintf(mosaico_diagnostics.battery, sizeof(mosaico_diagnostics.battery),
             "BAT READ:%s RC:%d UNCALIBRATED", first == 0 ? "PASS" : "FAIL", first);
}
static void audio_report(void *user, const h2_pal_audio_e2e_case_result_t *r) {
    (void)user;
    printf("H2_MOSAICO_AUDIO case=%s pass=%d blocked=%d detail=%d line=%u\n",
           r->id, r->passed, r->blocked, r->detail, r->line);
    fflush(stdout);
}
static void audio(h2_runtime_t *runtime) {
    step("AUDIO RECORD + PLAY 30 SECONDS");
    const h2_pal_audio_e2e_config_t config = {
        .audio = runtime->audio, .time = runtime->time,
        .stability_ms = 30000, .report = audio_report
    };
    h2_pal_audio_e2e_result_t result = {0};
    int rc = h2_pal_audio_e2e_run(&config, &result);
    mosaico_diagnostics.audio_passed = result.passed;
    mosaico_diagnostics.audio_failed = result.failed;
    mosaico_diagnostics.audio_blocked = result.blocked;
    printf("H2_MOSAICO_AUDIO_SUMMARY rc=%d pass=%u fail=%u blocked=%u mic_frames=%" PRIu32 " mic_peak=%" PRIu32 " mic_energy=%" PRIu64 " speaker_frames=%" PRIu32 " soak_ms=%" PRIu64 "\n",
           rc, result.passed, result.failed, result.blocked, result.mic_frames,
           result.mic_peak, result.mic_energy, result.speaker_frames, result.stability_elapsed_ms);
    snprintf(mosaico_diagnostics.audio, sizeof(mosaico_diagnostics.audio),
             "AUDIO P:%u F:%u BLOCK:%u", result.passed, result.failed, result.blocked);
    if (!result.cases[H2_PAL_AUDIO_E2E_CLEANUP].passed) {
        step("AUDIO CLEANUP FAILED - HALTED");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
static void codec_readback(const char *phase) {
    i2c_master_dev_handle_t device = NULL;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x19, .scl_speed_hz = 400000
    };
    int rc = i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &device);
    if (rc != 0) { record("codec_readback_open", rc); return; }
    const uint8_t regs[] = {0x00, 0x01, 0x02, 0x09, 0x0a, 0x0d, 0x12, 0x13, 0x31, 0x32, 0x37};
    for (size_t i = 0; i < sizeof(regs); ++i) {
        uint8_t value = 0;
        rc = i2c_master_transmit_receive(device, &regs[i], 1, &value, 1, 100);
        printf("H2_MOSAICO_CODEC phase=%s reg=%02x value=%02x rc=%d\n", phase, regs[i], value, rc);
    }
    record("codec_readback_close", i2c_master_bus_rm_device(device));
}
void mosaico_audible_test(h2_runtime_t *runtime) {
    step("LISTEN: 3 BEEPS / 100 PERCENT");
    h2_audio_info_t info = {0};
    uint32_t saved = 0;
    int rc = h2_pal_audio_get_info(runtime->audio, &info);
    if (rc == 0) rc = h2_pal_audio_get_speaker_volume_percent(runtime->audio, &saved);
    if (rc != 0) { record("audible_setup", rc); return; }
    const h2_audio_pcm_format_t f = info.playback_format;
    if (f.sample_format != H2_AUDIO_SAMPLE_S16LE || f.sample_rate_hz != 16000 ||
        f.channels == 0 || f.channels > 2 || f.frame_samples_per_channel == 0 ||
        f.frame_samples_per_channel > 512) {
        record("audible_format", H2_PAL_ERR_UNSUPPORTED); return;
    }
    rc = h2_pal_audio_set_speaker_volume_percent(runtime->audio, 100);
    bool started = false;
    h2_pal_audio_track_t *track = NULL;
    if (rc == 0) { rc = h2_pal_audio_start_speaker(runtime->audio); started = rc == 0; }
    const h2_audio_track_config_t config = {
        .name = "mosaico-audible", .format = f, .volume_factor_milli = 1000, .buffer_frames = 4
    };
    if (rc == 0) rc = h2_pal_audio_create_track(runtime->audio, &config, &track);
    if (rc == 0) codec_readback("tone_100_percent");
    static int16_t pcm[1024];
    const size_t bytes = f.frame_samples_per_channel * f.channels * sizeof(*pcm);
    h2_audio_frame_t frame = {.data = pcm, .capacity = sizeof(pcm), .bytes = bytes,
        .sample_rate_hz = f.sample_rate_hz, .samples_per_channel = f.frame_samples_per_channel,
        .channels = f.channels, .sample_format = f.sample_format};
    /* Three distinct frequencies, with silence between them; bounded amplitude. */
    for (unsigned beep = 0; rc == 0 && beep < 3; ++beep) {
        for (unsigned block = 0; rc == 0 && block < 20; ++block) {
            for (unsigned i = 0; i < f.frame_samples_per_channel; ++i) {
                const unsigned n = block * f.frame_samples_per_channel + i;
                const int16_t sample = block < 12 ? (int16_t)(4096 * sinf(
                    6.28318530718f * (600 + beep * 400) * n / f.sample_rate_hz)) : 0;
                for (unsigned ch = 0; ch < f.channels; ++ch) pcm[i * f.channels + ch] = sample;
            }
            rc = h2_pal_audio_track_write(track, &frame, 1000);
        }
    }
    if (rc == 0) rc = h2_pal_audio_track_drain(track, 3000);
    int cleanup = 0;
    if (track) cleanup = h2_pal_audio_track_close(track);
    if (cleanup == 0 && started) cleanup = h2_pal_audio_stop_speaker(runtime->audio);
    const int restore = h2_pal_audio_set_speaker_volume_percent(runtime->audio, saved);
    if (cleanup == 0) cleanup = restore;
    printf("H2_MOSAICO_AUDIBLE write_rc=%d cleanup_rc=%d heard=UNCONFIRMED\n", rc, cleanup);
    if (cleanup != 0) {
        step("TONE CLEANUP FAILED - HALTED");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    step(rc == 0 ? "BOOT: REPLAY BEEPS / HEARD?" : "TONE WRITE FAILED - CHECK LOG");
}

static bool wifi_result(void *user, const h2_pal_wifi_scan_entry_t *entry) {
    (void)user;
    if (entry) ++mosaico_diagnostics.wifi_aps;
    return true;
}
static void wifi(h2_runtime_t *runtime) {
    step("WIFI SCAN / AP START AND STOP");
    uint8_t mac[6];
    int rc = h2_pal_wifi_sta_get_mac(runtime->wifi_sta, mac);
    record("wifi_mac", rc);
    int first = rc;
    h2_pal_wifi_scan_request_t request = {0};
    rc = h2_pal_wifi_sta_scan(runtime->wifi_sta, &request, wifi_result, NULL, 15000);
    record("wifi_scan", rc);
    if (first == 0) first = rc;
    h2_pal_wifi_ap_config_t ap = {
        .ssid = "GizOS-Mosaico-Test", .ssid_len = 18,
        .password = "mosaico-test", .password_len = 12,
        .channel = 6, .max_clients = 1, .security = H2_PAL_WIFI_SECURITY_WPA2
    };
    ap.ssid_len = strlen(ap.ssid);
    ap.password_len = strlen(ap.password);
    rc = h2_pal_wifi_ap_start(runtime->wifi_ap, &ap, 5000);
    record("wifi_ap_start", rc);
    if (first == 0) first = rc;
    if (rc == 0) {
        h2_pal_wifi_ap_status_t status = {0};
        rc = h2_pal_wifi_ap_get_status(runtime->wifi_ap, &status);
        if (rc == 0 && status.state != H2_PAL_WIFI_AP_STATE_STARTED) rc = H2_PAL_ERR_INVALID_STATE;
        record("wifi_ap_status", rc);
        if (first == 0) first = rc;
        rc = h2_pal_wifi_ap_stop(runtime->wifi_ap, 5000);
        record("wifi_ap_stop", rc);
        if (first == 0) first = rc;
    }
    snprintf(mosaico_diagnostics.wifi, sizeof(mosaico_diagnostics.wifi), "WIFI:%s APS:%u RC:%d",
             first ? "FAIL" : mosaico_diagnostics.wifi_aps ? "SMOKE OK" : "NO RF DATA",
             mosaico_diagnostics.wifi_aps, first);
    printf("H2_MOSAICO_WIFI_SUMMARY %s\n", mosaico_diagnostics.wifi);
    printf("H2_MOSAICO_WIFI_SCOPE connection_dhcp_dns=NOT_TESTED\n");
}
static bool ble_result(void *user, const h2_pal_ble_scan_result_t *entry) {
    (void)user;
    if (entry) atomic_fetch_add(&ble_reports, 1);
    return false;
}
static void ble(h2_runtime_t *runtime) {
    step("BLE SCAN / ADVERTISING / STOP");
    const h2_pal_ble_host_api_t *api = runtime->ble_host;
    int rc = h2_pal_ble_start(api);
    int first = rc;
    record("ble_start", rc);
    if (rc == 0) {
        const h2_pal_ble_scan_params_t scan = {
            .mode = H2_PAL_BLE_SCAN_MODE_PASSIVE, .interval_ms = 100, .window_ms = 50,
            .timeout_ms = 0
        };
        rc = h2_pal_ble_start_scan(api, &scan, ble_result, NULL);
        record("ble_scan_start", rc);
        if (first == 0) first = rc;
        if (rc == 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            rc = h2_pal_ble_stop_scan(api);
            record("ble_scan_stop", rc);
            if (first == 0) first = rc;
        }
        const h2_pal_ble_adv_data_t data = {.local_name = "GizOS-Mosaico"};
        rc = h2_pal_ble_set_adv_data(api, &data);
        record("ble_adv_data", rc);
        if (first == 0) first = rc;
        if (rc == 0) {
            const h2_pal_ble_adv_params_t adv = {
                .mode = H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE,
                .interval_min_ms = 100, .interval_max_ms = 150
            };
            rc = h2_pal_ble_start_advertising(api, &adv);
            record("ble_adv_start", rc);
            if (first == 0) first = rc;
            if (rc == 0) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                rc = h2_pal_ble_stop_advertising(api);
                record("ble_adv_stop", rc);
                if (first == 0) first = rc;
            }
        }
        rc = h2_pal_ble_stop(api);
        record("ble_stop", rc);
        if (first == 0) first = rc;
    }
    mosaico_diagnostics.ble_reports = atomic_load(&ble_reports);
    snprintf(mosaico_diagnostics.ble, sizeof(mosaico_diagnostics.ble), "BLE:%s RX:%u RC:%d",
             first == H2_PAL_ERR_UNSUPPORTED ? "BLOCKED" : first ? "FAIL" :
             mosaico_diagnostics.ble_reports ? "SMOKE OK" : "NO RF DATA", mosaico_diagnostics.ble_reports, first);
    printf("H2_MOSAICO_BLE_SUMMARY %s\n", mosaico_diagnostics.ble);
    printf("H2_MOSAICO_BLE_SCOPE peer_connection_gatt=NOT_TESTED\n");
}
void mosaico_run_diagnostics(h2_runtime_t *runtime, void (*refresh)(void *), void *user) {
    refresh_screen = refresh;
    refresh_user = user;
    step("CAMERA SENSOR ID / LEFT SLOT");
    mosaico_camera_probe();
    magnetometers();
    battery(runtime);
    audio(runtime);
    wifi(runtime);
    ble(runtime);
    mosaico_audible_test(runtime);
    mosaico_camera_e2e_init();
}
