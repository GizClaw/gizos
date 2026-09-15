#include "h2_runtime.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port_fake.h"
#include <assert.h>
#include <stdio.h>

static unsigned delivered, started;
static int ble_listener(void *user, const h2_pal_system_event_t *event) {
    (void)user; (void)event; ++delivered; return H2_PAL_OK;
}
static void smoke(void *user) { (void)user; ++started; }
int main(void) {
    h2_jieli_fake_reset();
    const h2_pal_system_event_api_t *api = h2_jieli_wl82_platform_system_event_api();
    /* Shared launcher initializes before BLE subscribes. */
    assert(h2_pal_system_event_init(api) == H2_PAL_OK);
    h2_pal_system_event_subscription_t *ble = NULL;
    assert(h2_pal_system_event_subscribe(api, H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
                                      ble_listener, NULL, &ble) == H2_PAL_OK);
    const h2_runtime_config_t config = {
        .board = "jieli_ac791n_devkit", .target = "wl82", .chip = "ac791n",
        .firmware_info = h2_jieli_wl82_platform_firmware_info_api(),
        .mem = h2_jieli_wl82_platform_mem_api(),
        .log = h2_jieli_wl82_platform_log_api(),
        .time = h2_jieli_wl82_platform_time_api(),
        .timer = h2_jieli_wl82_platform_timer_api(),
        .task = h2_jieli_wl82_platform_task_api(),
        .queue = h2_jieli_wl82_platform_queue_api(),
        .sync = h2_jieli_wl82_platform_sync_api(),
        .fs = h2_pal_unsupported_fs_api(),
        .disk = h2_pal_unsupported_disk_api(),
        .pref = h2_pal_unsupported_pref_api(),
        .crypto = h2_pal_unsupported_crypto_api(),
        .http = h2_pal_unsupported_http_api(),
        .net = h2_pal_unsupported_net_api(),
        .netif = h2_pal_unsupported_netif_api(),
        .mqtt = h2_pal_unsupported_mqtt_api(),
        .webrtc = h2_pal_unsupported_webrtc_api(),
        .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
        .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
        .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
        .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
        .ble_host = h2_pal_unsupported_ble_host_api(),
        .modem = h2_pal_unsupported_modem_api(),
        .power = h2_pal_unsupported_power_api(),
        .display = h2_pal_unsupported_display_api(),
        .audio = h2_pal_unsupported_audio_api(),
        .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
        .periph = h2_pal_unsupported_periph_api(),
        .button = h2_pal_unsupported_button_api(),
        .touch = h2_pal_unsupported_touch_api(),
        .buzzer = h2_pal_unsupported_buzzer_api(),
        .nfc = h2_pal_unsupported_nfc_api(),
        .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
        .imu = h2_pal_unsupported_imu_api(),
        .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
        .led = h2_pal_unsupported_led_api(),
        .switch_api = h2_pal_unsupported_switch_api(),
        .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
        .input = h2_pal_unsupported_input_api(),
        .system_event = h2_jieli_wl82_platform_system_event_api(),
        .video_decoder = h2_pal_unsupported_video_decoder_api(),
    };
    h2_runtime_t *runtime = NULL;
    const int result = h2_runtime_init(&config, &runtime);
    printf("runtime-init=%d\n", result);
    assert(result == H2_PAL_OK);
    /* Runtime init must retain the launcher's existing BLE subscription. */
    const h2_pal_system_event_t event = {.type = H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED};
    assert(h2_pal_system_event_post(api, &event, 0) == H2_PAL_OK);
    assert(delivered == 1);
    const h2_pal_task_options_t options = {.name = "smoke", .min_stack_size = 4096};
    h2_pal_task_t *task = NULL;
    assert(h2_pal_task_start(runtime->task, &options, smoke, NULL, &task) == H2_PAL_OK);
    h2_jieli_fake_run_last_task_once();
    assert(started == 1);
    assert(h2_pal_task_join(runtime->task, task) == H2_PAL_OK);
    /* Stop the launcher subscriber before Runtime tears down the registry. */
    h2_pal_system_event_unsubscribe(api, ble);
    h2_runtime_deinit(runtime);
    assert(h2_jieli_fake_live_allocations() == 0);
    return 0;
}
