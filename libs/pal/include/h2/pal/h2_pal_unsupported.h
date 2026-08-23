#ifndef H2_PAL_UNSUPPORTED_H
#define H2_PAL_UNSUPPORTED_H

/* Canonical API objects for capabilities that a platform does not support. */

#include "h2_pal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_DECLARE_UNSUPPORTED_API(name, type) \
    const type *h2_pal_unsupported_##name##_api(void)

H2_PAL_DECLARE_UNSUPPORTED_API(audio, h2_pal_audio_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(audio_decoder, h2_pal_audio_decoder_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(ble_host, h2_pal_ble_host_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(button, h2_pal_button_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(touch, h2_pal_touch_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(buzzer, h2_pal_buzzer_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(crypto, h2_pal_crypto_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(disk, h2_pal_disk_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(dtls, h2_pal_dtls_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(display, h2_pal_display_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(fs, h2_pal_fs_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(firmware_info, h2_pal_firmware_info_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(gpio_irq, h2_pal_gpio_irq_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(http, h2_pal_http_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(imu, h2_pal_imu_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(input, h2_pal_input_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(json, h2_pal_json_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(led, h2_pal_led_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(log, h2_pal_log_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(mem, h2_pal_mem_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(modem, h2_pal_modem_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(mqtt, h2_pal_mqtt_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(net, h2_pal_net_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(netif, h2_pal_netif_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(nfc, h2_pal_nfc_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(
    nfc_card_emulation,
    h2_pal_nfc_card_emulation_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(periph, h2_pal_periph_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(power, h2_pal_power_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(pref, h2_pal_pref_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(pwm_switch, h2_pal_pwm_switch_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(queue, h2_pal_queue_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(sctp, h2_pal_sctp_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(serial_host, h2_pal_serial_host_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(switch, h2_pal_switch_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(sync, h2_pal_sync_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(system_event, h2_pal_system_event_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(task, h2_pal_task_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(time, h2_pal_time_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(timer, h2_pal_timer_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(uart_io_stream, h2_pal_uart_io_stream_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(usb_jtag_io_stream, h2_pal_usb_jtag_io_stream_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(video_decoder, h2_pal_video_decoder_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(webrtc, h2_pal_webrtc_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(wifi_ap, h2_pal_wifi_ap_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(wifi_csi, h2_pal_wifi_csi_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(wifi_settings, h2_pal_wifi_settings_api_t);
H2_PAL_DECLARE_UNSUPPORTED_API(wifi_sta, h2_pal_wifi_sta_api_t);

#undef H2_PAL_DECLARE_UNSUPPORTED_API

#ifdef __cplusplus
}
#endif

#endif
