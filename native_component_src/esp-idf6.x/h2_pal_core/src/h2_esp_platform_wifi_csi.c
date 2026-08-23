#include "h2_esp_platform_core.h"

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define H2_ESP_WIFI_CSI_MAX_SAMPLES 128u

typedef struct h2_esp_wifi_csi_state {
  portMUX_TYPE lock;
  h2_pal_wifi_csi_frame_fn frame_cb;
  void *frame_user;
  uint32_t min_delivery_interval_ms;
  int64_t last_delivery_us;
  uint32_t callback_in_flight;
  int active;
} h2_esp_wifi_csi_state_t;

static h2_esp_wifi_csi_state_t s_h2_esp_wifi_csi = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static h2_pal_result_t h2_esp_wifi_csi_map_error(esp_err_t error) {
  if (error == ESP_OK) {
    return H2_PAL_OK;
  }
  if (error == ESP_ERR_NO_MEM) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  if (error == ESP_ERR_INVALID_ARG) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (error == ESP_ERR_WIFI_NOT_INIT || error == ESP_ERR_WIFI_NOT_STARTED) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  if (error == ESP_ERR_WIFI_NOT_CONNECT || error == ESP_ERR_WIFI_STATE) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return H2_PAL_ERR_IO;
}

static h2_pal_wifi_csi_phy_t h2_esp_wifi_csi_phy(unsigned int sig_mode) {
  if (sig_mode == 0u) {
    return H2_PAL_WIFI_CSI_PHY_NON_HT;
  }
  if (sig_mode == 1u) {
    return H2_PAL_WIFI_CSI_PHY_HT;
  }
  if (sig_mode == 3u) {
    return H2_PAL_WIFI_CSI_PHY_VHT;
  }
  return H2_PAL_WIFI_CSI_PHY_UNKNOWN;
}

static void h2_esp_wifi_csi_receive(void *context, wifi_csi_info_t *data) {
  h2_esp_wifi_csi_state_t *state = context;
  h2_pal_wifi_csi_frame_fn frame_cb;
  void *frame_user;
  uint32_t min_delivery_interval_ms;
  int64_t now_us;
  size_t offset;
  size_t sample_count;

  if (state == NULL || data == NULL || data->buf == NULL) {
    return;
  }

  offset = data->first_word_invalid ? 4u : 0u;
  if ((size_t)data->len <= offset) {
    return;
  }
  sample_count = ((size_t)data->len - offset) / 2u;
  if (sample_count == 0u) {
    return;
  }
  if (sample_count > H2_ESP_WIFI_CSI_MAX_SAMPLES) {
    sample_count = H2_ESP_WIFI_CSI_MAX_SAMPLES;
  }

  now_us = esp_timer_get_time();
  portENTER_CRITICAL(&state->lock);
  frame_cb = state->frame_cb;
  frame_user = state->frame_user;
  min_delivery_interval_ms = state->min_delivery_interval_ms;
  if (!state->active || frame_cb == NULL ||
      (min_delivery_interval_ms > 0u &&
       now_us - state->last_delivery_us <
           (int64_t)min_delivery_interval_ms * 1000LL)) {
    portEXIT_CRITICAL(&state->lock);
    return;
  }
  state->last_delivery_us = now_us;
  portEXIT_CRITICAL(&state->lock);

  h2_pal_wifi_csi_sample_t samples[H2_ESP_WIFI_CSI_MAX_SAMPLES];
  for (size_t index = 0u; index < sample_count; ++index) {
    samples[index].imag = data->buf[offset + index * 2u];
    samples[index].real = data->buf[offset + index * 2u + 1u];
  }

  h2_pal_wifi_csi_frame_t frame = {
      .provider = H2_PAL_WIFI_CSI_PROVIDER_ESP_IDF,
      .phy = h2_esp_wifi_csi_phy(data->rx_ctrl.sig_mode),
      .channel = data->rx_ctrl.channel,
      .bandwidth_mhz = data->rx_ctrl.cwb ? 40u : 20u,
      .mcs = data->rx_ctrl.mcs,
      .rssi_dbm = data->rx_ctrl.rssi,
      .samples = samples,
      .sample_count = sample_count,
  };
  portENTER_CRITICAL(&state->lock);
  if (!state->active || state->frame_cb != frame_cb ||
      state->frame_user != frame_user) {
    portEXIT_CRITICAL(&state->lock);
    return;
  }
  state->callback_in_flight++;
  portEXIT_CRITICAL(&state->lock);
  frame_cb(frame_user, &frame);

  portENTER_CRITICAL(&state->lock);
  state->callback_in_flight--;
  portEXIT_CRITICAL(&state->lock);
}

static void h2_esp_wifi_csi_wait_for_callbacks(
    h2_esp_wifi_csi_state_t *state) {
  for (;;) {
    uint32_t callback_in_flight;

    portENTER_CRITICAL(&state->lock);
    callback_in_flight = state->callback_in_flight;
    portEXIT_CRITICAL(&state->lock);
    if (callback_in_flight == 0u) {
      return;
    }
    vTaskDelay(1);
  }
}

static h2_pal_result_t h2_esp_wifi_csi_get_capabilities(
    void *user, h2_pal_wifi_csi_capabilities_t *out_capabilities) {
  (void)user;
  out_capabilities->provider = H2_PAL_WIFI_CSI_PROVIDER_ESP_IDF;
  out_capabilities->max_sample_count = H2_ESP_WIFI_CSI_MAX_SAMPLES;
  return H2_PAL_OK;
}

static h2_pal_result_t
h2_esp_wifi_csi_start(void *user, const h2_pal_wifi_csi_config_t *config,
                      h2_pal_wifi_csi_frame_fn frame_cb, void *frame_user) {
  h2_esp_wifi_csi_state_t *state = user;
  wifi_ap_record_t ap_info;
  wifi_csi_config_t esp_config = {
      .lltf_en = true,
      .htltf_en = true,
      .stbc_htltf2_en = true,
      .ltf_merge_en = true,
      .channel_filter_en = false,
      .manu_scale = false,
      .shift = 0u,
      .dump_ack_en = false,
  };
  h2_pal_result_t rc;

  if (state == NULL || config == NULL || frame_cb == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  rc = h2_esp_platform_wifi_ensure_started();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  memset(&ap_info, 0, sizeof(ap_info));
  rc = h2_esp_wifi_csi_map_error(esp_wifi_sta_get_ap_info(&ap_info));
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (config->bssid_set != 0u &&
      memcmp(config->bssid, ap_info.bssid, sizeof(ap_info.bssid)) != 0) {
    return H2_PAL_ERR_NOT_FOUND;
  }

  portENTER_CRITICAL(&state->lock);
  if (state->active) {
    portEXIT_CRITICAL(&state->lock);
    return H2_PAL_ERR_INVALID_STATE;
  }
  state->frame_cb = frame_cb;
  state->frame_user = frame_user;
  state->min_delivery_interval_ms = config->min_delivery_interval_ms;
  state->last_delivery_us = 0;
  state->active = 1;
  portEXIT_CRITICAL(&state->lock);

  rc = h2_esp_wifi_csi_map_error(esp_wifi_set_csi_config(&esp_config));
  if (rc == H2_PAL_OK) {
    rc = h2_esp_wifi_csi_map_error(
        esp_wifi_set_csi_rx_cb(h2_esp_wifi_csi_receive, state));
  }
  if (rc == H2_PAL_OK) {
    rc = h2_esp_wifi_csi_map_error(esp_wifi_set_csi(true));
  }
  if (rc == H2_PAL_OK) {
    return H2_PAL_OK;
  }

  (void)esp_wifi_set_csi(false);
  (void)esp_wifi_set_csi_rx_cb(NULL, NULL);
  portENTER_CRITICAL(&state->lock);
  state->frame_cb = NULL;
  state->frame_user = NULL;
  state->active = 0;
  portEXIT_CRITICAL(&state->lock);
  h2_esp_wifi_csi_wait_for_callbacks(state);
  return rc;
}

static h2_pal_result_t h2_esp_wifi_csi_stop(void *user) {
  h2_esp_wifi_csi_state_t *state = user;
  h2_pal_result_t rc;
  int was_active;

  if (state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  portENTER_CRITICAL(&state->lock);
  was_active = state->active;
  state->frame_cb = NULL;
  state->frame_user = NULL;
  state->active = 0;
  portEXIT_CRITICAL(&state->lock);

  if (!was_active) {
    return H2_PAL_OK;
  }

  rc = h2_esp_wifi_csi_map_error(esp_wifi_set_csi(false));
  (void)esp_wifi_set_csi_rx_cb(NULL, NULL);
  h2_esp_wifi_csi_wait_for_callbacks(state);
  return rc;
}

static const h2_pal_wifi_csi_vtable_t s_h2_esp_wifi_csi_vtable = {
    .get_capabilities = h2_esp_wifi_csi_get_capabilities,
    .start = h2_esp_wifi_csi_start,
    .stop = h2_esp_wifi_csi_stop,
};

static const h2_pal_wifi_csi_api_t s_h2_esp_wifi_csi_api = {
    .user = &s_h2_esp_wifi_csi,
    .vtable = &s_h2_esp_wifi_csi_vtable,
};

const h2_pal_wifi_csi_api_t *h2_esp_platform_wifi_csi(void) {
  return &s_h2_esp_wifi_csi_api;
}
