#include "h2_bk_platform_core.h"

#include <common/bk_err.h>
#include <components/log.h>
#include <modules/wifi.h>
#include <os/os.h>

#include <string.h>

#define H2_BK_WIFI_CSI_MAX_SAMPLES 52u
#define H2_BK_WIFI_CSI_MIN_INTERVAL_MS 10u
#define H2_BK_WIFI_CSI_HOST_CAPTURE_MODE 0x01u
#define H2_BK_WIFI_CSI_STA_IDENTITY 0x01u
#define H2_BK_WIFI_CSI_NON_HT_HT_FORMAT 0x03u
#define H2_BK_WIFI_CSI_NON_HT_RX_FORMAT 0x00u
#define H2_BK_WIFI_CSI_HT_RX_FORMAT 0x02u
#define H2_BK_WIFI_CSI_START_DELAY 5u
#define H2_BK_WIFI_CSI_DIAGNOSTIC_FRAME_LIMIT 4u

typedef struct h2_bk_wifi_csi_state {
  h2_pal_wifi_csi_frame_fn frame_cb;
  void *frame_user;
  uint8_t bssid[6];
  uint8_t bssid_set;
  uint32_t min_delivery_interval_ms;
  uint32_t last_delivery_ms;
  uint32_t callback_in_flight;
  uint32_t diagnostic_frame_count;
  uint32_t diagnostic_rejected_count;
  int active;
} h2_bk_wifi_csi_state_t;

static h2_bk_wifi_csi_state_t s_h2_bk_wifi_csi;

static h2_pal_result_t h2_bk_wifi_csi_map_error(bk_err_t error) {
  switch (error) {
  case BK_OK:
    return H2_PAL_OK;
  case BK_ERR_PARAM:
  case BK_ERR_NULL_PARAM:
    return H2_PAL_ERR_INVALID_ARG;
  case BK_ERR_NO_MEM:
    return H2_PAL_ERR_NO_MEMORY;
  case BK_ERR_TIMEOUT:
    return H2_PAL_ERR_TIMEOUT;
  case BK_ERR_NOT_SUPPORT:
    return H2_PAL_ERR_UNSUPPORTED;
  case BK_ERR_BUSY:
  case BK_ERR_IN_PROGRESS:
  case BK_ERR_STATE:
    return H2_PAL_ERR_INVALID_STATE;
  case BK_ERR_WIFI_NOT_INIT:
    return H2_PAL_ERR_UNAVAILABLE;
  default:
    return H2_PAL_ERR_IO;
  }
}

static h2_pal_wifi_csi_phy_t h2_bk_wifi_csi_phy(uint8_t format) {
  switch (format) {
  case H2_BK_WIFI_CSI_NON_HT_RX_FORMAT:
    return H2_PAL_WIFI_CSI_PHY_NON_HT;
  case H2_BK_WIFI_CSI_HT_RX_FORMAT:
    return H2_PAL_WIFI_CSI_PHY_HT;
  case 0x04u:
    return H2_PAL_WIFI_CSI_PHY_VHT;
  case 0x05u:
    return H2_PAL_WIFI_CSI_PHY_HE;
  default:
    return H2_PAL_WIFI_CSI_PHY_UNKNOWN;
  }
}

static int16_t h2_bk_wifi_csi_signed_13(uint32_t value) {
  int32_t sample = (int32_t)(value & 0x1fffu);
  if (sample >= 4096) {
    sample -= 8192;
  }
  return (int16_t)sample;
}

static void h2_bk_wifi_csi_callback_done(h2_bk_wifi_csi_state_t *state) {
  uint32_t interrupt_level = rtos_enter_critical();
  state->callback_in_flight--;
  rtos_exit_critical(interrupt_level);
}

static void h2_bk_wifi_csi_receive(struct wifi_csi_info_t *info) {
  h2_bk_wifi_csi_state_t *state = &s_h2_bk_wifi_csi;
  h2_pal_wifi_csi_frame_fn frame_cb;
  void *frame_user;
  uint32_t interrupt_level;
  uint32_t now_ms;
  uint32_t diagnostic_frame_count;
  uint32_t diagnostic_rejected_count;

  if (info == NULL) {
    return;
  }
  interrupt_level = rtos_enter_critical();
  frame_cb = state->frame_cb;
  frame_user = state->frame_user;
  if (!state->active || frame_cb == NULL) {
    rtos_exit_critical(interrupt_level);
    return;
  }
  state->callback_in_flight++;
  rtos_exit_critical(interrupt_level);

  /*
   * The BK LMAC stores the transmitted probe frame's destination address in
   * wifi_csi_info_t.mac. In STA mode that address is the associated BSSID.
   * Matching it also filters BK_EVT_BCN_CC_RXED payloads that the fixed SDK's
   * AP event switch accidentally forwards to the CSI callback.
   */
  if (state->bssid_set != 0u &&
      memcmp(state->bssid, info->mac, sizeof(state->bssid)) != 0) {
    h2_bk_wifi_csi_callback_done(state);
    return;
  }
  if (info->data_type != 0u || info->len == 0u ||
      info->len > H2_BK_WIFI_CSI_MAX_SAMPLES ||
      (info->rx_ctrl.foramat != H2_BK_WIFI_CSI_NON_HT_RX_FORMAT &&
       info->rx_ctrl.foramat != H2_BK_WIFI_CSI_HT_RX_FORMAT)) {
    interrupt_level = rtos_enter_critical();
    state->diagnostic_rejected_count++;
    diagnostic_rejected_count = state->diagnostic_rejected_count;
    rtos_exit_critical(interrupt_level);
    if (diagnostic_rejected_count <= H2_BK_WIFI_CSI_DIAGNOSTIC_FRAME_LIMIT) {
      BK_LOGW(
          "h2_csi", "drop=%u len=%u type=%u format=%u channel=%u rssi=%d\r\n",
          (unsigned int)diagnostic_rejected_count, (unsigned int)info->len,
          (unsigned int)info->data_type, (unsigned int)info->rx_ctrl.foramat,
          (unsigned int)info->rx_ctrl.channel, (int)info->rx_ctrl.rssi);
    }
    h2_bk_wifi_csi_callback_done(state);
    return;
  }
  now_ms = (uint32_t)rtos_get_time();
  interrupt_level = rtos_enter_critical();
  if (!state->active ||
      (state->min_delivery_interval_ms > 0u &&
       now_ms - state->last_delivery_ms < state->min_delivery_interval_ms)) {
    rtos_exit_critical(interrupt_level);
    h2_bk_wifi_csi_callback_done(state);
    return;
  }
  state->last_delivery_ms = now_ms;
  state->diagnostic_frame_count++;
  diagnostic_frame_count = state->diagnostic_frame_count;
  rtos_exit_critical(interrupt_level);

  if (diagnostic_frame_count <= H2_BK_WIFI_CSI_DIAGNOSTIC_FRAME_LIMIT) {
    BK_LOGI("h2_csi",
            "frame=%u len=%u type=%u format=%u channel=%u rssi=%d "
            "mac=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
            (unsigned int)diagnostic_frame_count, (unsigned int)info->len,
            (unsigned int)info->data_type, (unsigned int)info->rx_ctrl.foramat,
            (unsigned int)info->rx_ctrl.channel, (int)info->rx_ctrl.rssi,
            (unsigned int)info->mac[0], (unsigned int)info->mac[1],
            (unsigned int)info->mac[2], (unsigned int)info->mac[3],
            (unsigned int)info->mac[4], (unsigned int)info->mac[5]);
  }

  h2_pal_wifi_csi_sample_t samples[H2_BK_WIFI_CSI_MAX_SAMPLES];
  size_t sample_count = (size_t)info->len;
  for (size_t index = 0u; index < sample_count; ++index) {
    uint32_t packed = info->data.buf[index];
    samples[index].imag = h2_bk_wifi_csi_signed_13(packed);
    samples[index].real = h2_bk_wifi_csi_signed_13(packed >> 13u);
  }

  h2_pal_wifi_csi_frame_t frame = {
      .provider = H2_PAL_WIFI_CSI_PROVIDER_BK7258,
      .phy = h2_bk_wifi_csi_phy(info->rx_ctrl.foramat),
      .channel = info->rx_ctrl.channel,
      .bandwidth_mhz = info->rx_ctrl.cwb != 0u ? 40u : 20u,
      .mcs = info->rx_ctrl.mcs,
      .rssi_dbm = info->rx_ctrl.rssi,
      .samples = sample_count > 0u ? samples : NULL,
      .sample_count = sample_count,
  };
  frame_cb(frame_user, &frame);

  h2_bk_wifi_csi_callback_done(state);
}

static void h2_bk_wifi_csi_wait_for_callbacks(h2_bk_wifi_csi_state_t *state) {
  for (;;) {
    uint32_t callback_in_flight;
    uint32_t interrupt_level = rtos_enter_critical();
    callback_in_flight = state->callback_in_flight;
    rtos_exit_critical(interrupt_level);
    if (callback_in_flight == 0u) {
      return;
    }
    (void)rtos_delay_milliseconds(1u);
  }
}

static h2_pal_result_t h2_bk_wifi_csi_get_capabilities(
    void *user, h2_pal_wifi_csi_capabilities_t *out_capabilities) {
  (void)user;
  if (out_capabilities == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_capabilities->provider = H2_PAL_WIFI_CSI_PROVIDER_BK7258;
#if !H2_BK_PLATFORM_WIFI_CSI_SAFE_DISPATCH
  out_capabilities->max_sample_count = 0u;
  return H2_PAL_ERR_UNAVAILABLE;
#else
  out_capabilities->max_sample_count = H2_BK_WIFI_CSI_MAX_SAMPLES;
  return H2_PAL_OK;
#endif
}

static h2_pal_result_t
h2_bk_wifi_csi_start(void *user, const h2_pal_wifi_csi_config_t *config,
                     h2_pal_wifi_csi_frame_fn frame_cb, void *frame_user) {
  h2_bk_wifi_csi_state_t *state = user;
#if H2_BK_PLATFORM_WIFI_CSI_SAFE_DISPATCH
  wifi_link_status_t link_status;
  bk_err_t vendor_rc;
  uint32_t interrupt_level;
  uint32_t delivery_interval_ms;
  h2_pal_result_t rc;
#endif

  if (state == NULL || config == NULL || frame_cb == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
#if !H2_BK_PLATFORM_WIFI_CSI_SAFE_DISPATCH
  return H2_PAL_ERR_UNAVAILABLE;
#else
  memset(&link_status, 0, sizeof(link_status));
  vendor_rc = bk_wifi_sta_get_link_status(&link_status);
  rc = h2_bk_wifi_csi_map_error(vendor_rc);
  if (rc != H2_PAL_OK) {
    BK_LOGE("h2_csi", "stage=link_status vendor_rc=%d pal_rc=%d\r\n",
            (int)vendor_rc, (int)rc);
    return rc;
  }
  if (link_status.state != WIFI_LINKSTATE_STA_CONNECTED &&
      link_status.state != WIFI_LINKSTATE_STA_GOT_IP) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (config->bssid_set != 0u && memcmp(config->bssid, link_status.bssid,
                                        sizeof(link_status.bssid)) != 0) {
    return H2_PAL_ERR_NOT_FOUND;
  }

  interrupt_level = rtos_enter_critical();
  if (state->active) {
    rtos_exit_critical(interrupt_level);
    return H2_PAL_ERR_INVALID_STATE;
  }
  state->frame_cb = frame_cb;
  state->frame_user = frame_user;
  state->bssid_set = 1u;
  memcpy(state->bssid, link_status.bssid, sizeof(state->bssid));
  state->min_delivery_interval_ms = config->min_delivery_interval_ms;
  state->last_delivery_ms = 0u;
  state->diagnostic_frame_count = 0u;
  state->diagnostic_rejected_count = 0u;
  state->active = 1;
  rtos_exit_critical(interrupt_level);

  delivery_interval_ms = config->min_delivery_interval_ms;
  if (delivery_interval_ms < H2_BK_WIFI_CSI_MIN_INTERVAL_MS) {
    delivery_interval_ms = H2_BK_WIFI_CSI_MIN_INTERVAL_MS;
  }
  BK_LOGI("h2_csi", "register interval=%u channel=%u\r\n",
          (unsigned int)delivery_interval_ms,
          (unsigned int)link_status.channel);
  bk_wifi_csi_info_cb_register(h2_bk_wifi_csi_receive);
  vendor_rc = bk_wifi_csi_start_req(
      0u, H2_BK_WIFI_CSI_HOST_CAPTURE_MODE, H2_BK_WIFI_CSI_STA_IDENTITY,
      H2_BK_WIFI_CSI_NON_HT_HT_FORMAT, delivery_interval_ms,
      H2_BK_WIFI_CSI_START_DELAY);
  rc = h2_bk_wifi_csi_map_error(vendor_rc);
  BK_LOGI("h2_csi", "stage=start vendor_rc=%d pal_rc=%d\r\n", (int)vendor_rc,
          (int)rc);
  if (rc == H2_PAL_OK) {
    return H2_PAL_OK;
  }

  bk_wifi_csi_info_cb_register(NULL);
  interrupt_level = rtos_enter_critical();
  state->frame_cb = NULL;
  state->frame_user = NULL;
  state->active = 0;
  rtos_exit_critical(interrupt_level);
  h2_bk_wifi_csi_wait_for_callbacks(state);
  return rc;
#endif
}

static h2_pal_result_t h2_bk_wifi_csi_stop(void *user) {
  h2_bk_wifi_csi_state_t *state = user;
#if H2_BK_PLATFORM_WIFI_CSI_SAFE_DISPATCH
  bk_err_t vendor_rc;
  uint32_t interrupt_level;
  int was_active;
  h2_pal_result_t rc;
#endif

  if (state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
#if !H2_BK_PLATFORM_WIFI_CSI_SAFE_DISPATCH
  return H2_PAL_ERR_UNAVAILABLE;
#else
  bk_wifi_csi_info_cb_register(NULL);
  interrupt_level = rtos_enter_critical();
  was_active = state->active;
  state->frame_cb = NULL;
  state->frame_user = NULL;
  state->active = 0;
  rtos_exit_critical(interrupt_level);
  if (!was_active) {
    return H2_PAL_OK;
  }

  vendor_rc = bk_wifi_csi_stop_req();
  rc = h2_bk_wifi_csi_map_error(vendor_rc);
  BK_LOGI("h2_csi", "stage=stop vendor_rc=%d pal_rc=%d\r\n", (int)vendor_rc,
          (int)rc);
  h2_bk_wifi_csi_wait_for_callbacks(state);
  return rc;
#endif
}

static const h2_pal_wifi_csi_vtable_t s_h2_bk_wifi_csi_vtable = {
    .get_capabilities = h2_bk_wifi_csi_get_capabilities,
    .start = h2_bk_wifi_csi_start,
    .stop = h2_bk_wifi_csi_stop,
};

static const h2_pal_wifi_csi_api_t s_h2_bk_wifi_csi_api = {
    .user = &s_h2_bk_wifi_csi,
    .vtable = &s_h2_bk_wifi_csi_vtable,
};

const h2_pal_wifi_csi_api_t *h2_bk_platform_wifi_csi(void) {
  return &s_h2_bk_wifi_csi_api;
}
