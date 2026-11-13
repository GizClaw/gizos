#include "h2_fruit_slice_native_host.h"
#include "h2_pal_unsupported.h"
#include <string.h>

h2_runtime_config_t h2_fruit_slice_native_defaults(void) {
  h2_runtime_config_t config = {0};
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.mem = h2_pal_unsupported_mem_api();
  config.log = h2_pal_unsupported_log_api();
  config.time = h2_pal_unsupported_time_api();
  config.timer = h2_pal_unsupported_timer_api();
  config.task = h2_pal_unsupported_task_api();
  config.queue = h2_pal_unsupported_queue_api();
  config.sync = h2_pal_unsupported_sync_api();
  config.fs = h2_pal_unsupported_fs_api();
  config.disk = h2_pal_unsupported_disk_api();
  config.pref = h2_pal_unsupported_pref_api();
  config.crypto = h2_pal_unsupported_crypto_api();
  config.http = h2_pal_unsupported_http_api();
  config.net = h2_pal_unsupported_net_api();
  config.netif = h2_pal_unsupported_netif_api();
  config.mqtt = h2_pal_unsupported_mqtt_api();
  config.webrtc = h2_pal_unsupported_webrtc_api();
  config.wifi_sta = h2_pal_unsupported_wifi_sta_api();
  config.wifi_ap = h2_pal_unsupported_wifi_ap_api();
  config.wifi_csi = h2_pal_unsupported_wifi_csi_api();
  config.wifi_settings = h2_pal_unsupported_wifi_settings_api();
  config.ble_host = h2_pal_unsupported_ble_host_api();
  config.modem = h2_pal_unsupported_modem_api();
  config.power = h2_pal_unsupported_power_api();
  config.camera = h2_pal_unsupported_camera_api();
  config.display = h2_pal_unsupported_display_api();
  config.audio = h2_pal_unsupported_audio_api();
  config.audio_decoder = h2_pal_unsupported_audio_decoder_api();
  config.periph = h2_pal_unsupported_periph_api();
  config.button = h2_pal_unsupported_button_api();
  config.touch = h2_pal_unsupported_touch_api();
  config.buzzer = h2_pal_unsupported_buzzer_api();
  config.nfc = h2_pal_unsupported_nfc_api();
  config.nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api();
  config.imu = h2_pal_unsupported_imu_api();
  config.gpio_irq = h2_pal_unsupported_gpio_irq_api();
  config.led = h2_pal_unsupported_led_api();
  config.switch_api = h2_pal_unsupported_switch_api();
  config.pwm_switch = h2_pal_unsupported_pwm_switch_api();
  config.input = h2_pal_unsupported_input_api();
  config.system_event = h2_pal_unsupported_system_event_api();
  config.video_decoder = h2_pal_unsupported_video_decoder_api();
  return config;
}

static h2_pal_result_t pointer_open(void* user) {
  h2_fruit_slice_pointer_t* pointer = user;
  if (!pointer->opened) {
    pointer->pressed = false;
    pointer->opened = true;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t pointer_info(void* user, h2_pal_touch_info_t* out) {
  h2_fruit_slice_pointer_t* pointer = user;
  if (!pointer->opened) return H2_PAL_ERR_INVALID_STATE;
  *out = (h2_pal_touch_info_t){pointer->width, pointer->height};
  return H2_PAL_OK;
}
static h2_pal_result_t pointer_poll(void* user, h2_pal_touch_event_t* out) {
  h2_fruit_slice_pointer_t* pointer = user;
  memset(out, 0, sizeof(*out));
  if (!pointer->opened) return H2_PAL_ERR_INVALID_STATE;
  int32_t x = 0, y = 0;
  int pressed = 0;
  const h2_pal_result_t rc = pointer->read(pointer->user, &x, &y, &pressed);
  if (rc != H2_PAL_OK) return rc;
  if (pressed) {
    if (x < 0 || y < 0 || x >= pointer->width || y >= pointer->height)
      return H2_PAL_ERR_IO;
    if (pointer->pressed && x == pointer->last_x && y == pointer->last_y)
      return H2_PAL_ERR_WOULD_BLOCK;
    *out = (h2_pal_touch_event_t){
        pointer->pressed ? H2_PAL_TOUCH_EVENT_MOVE : H2_PAL_TOUCH_EVENT_DOWN, x,
        y};
    pointer->last_x = x;
    pointer->last_y = y;
    pointer->pressed = true;
    return H2_PAL_OK;
  }
  if (!pointer->pressed) return H2_PAL_ERR_WOULD_BLOCK;
  *out = (h2_pal_touch_event_t){H2_PAL_TOUCH_EVENT_UP, pointer->last_x,
                                pointer->last_y};
  pointer->pressed = false;
  return H2_PAL_OK;
}
static h2_pal_result_t pointer_close(void* user) {
  h2_fruit_slice_pointer_t* pointer = user;
  pointer->opened = pointer->pressed = false;
  return H2_PAL_OK;
}
static const h2_pal_touch_vtable_t pointer_vtable = {.open = pointer_open,
                                                     .get_info = pointer_info,
                                                     .poll_event = pointer_poll,
                                                     .close = pointer_close};
h2_pal_result_t h2_fruit_slice_pointer_init(h2_fruit_slice_pointer_t* pointer,
                                            int32_t width, int32_t height,
                                            h2_fruit_slice_pointer_read_fn read,
                                            void* user) {
  if (!pointer || width <= 0 || height <= 0 || !read)
    return H2_PAL_ERR_INVALID_ARG;
  memset(pointer, 0, sizeof(*pointer));
  pointer->width = width;
  pointer->height = height;
  pointer->read = read;
  pointer->user = user;
  pointer->api = (h2_pal_touch_api_t){pointer, &pointer_vtable};
  return H2_PAL_OK;
}
const h2_pal_touch_api_t* h2_fruit_slice_pointer_touch(
    h2_fruit_slice_pointer_t* pointer) {
  return pointer ? &pointer->api : NULL;
}
