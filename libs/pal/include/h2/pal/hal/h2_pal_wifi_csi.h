#ifndef H2_PAL_WIFI_CSI_H
#define H2_PAL_WIFI_CSI_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Identifies the platform backend that produced a CSI capability or frame. */
typedef enum h2_pal_wifi_csi_provider {
  H2_PAL_WIFI_CSI_PROVIDER_UNKNOWN = 0,
  H2_PAL_WIFI_CSI_PROVIDER_ESP_IDF,
  H2_PAL_WIFI_CSI_PROVIDER_BK7258,
  H2_PAL_WIFI_CSI_PROVIDER_FAKE,
} h2_pal_wifi_csi_provider_t;

/** Identifies the 802.11 PHY mode reported with a CSI frame. */
typedef enum h2_pal_wifi_csi_phy {
  H2_PAL_WIFI_CSI_PHY_UNKNOWN = 0,
  H2_PAL_WIFI_CSI_PHY_NON_HT,
  H2_PAL_WIFI_CSI_PHY_HT,
  H2_PAL_WIFI_CSI_PHY_VHT,
  H2_PAL_WIFI_CSI_PHY_HE,
} h2_pal_wifi_csi_phy_t;

/** One complex CSI sample, expressed as signed real and imaginary values. */
typedef struct h2_pal_wifi_csi_sample {
  int16_t real;
  int16_t imag;
} h2_pal_wifi_csi_sample_t;

/** Describes a CSI provider's stable limits. */
typedef struct h2_pal_wifi_csi_capabilities {
  h2_pal_wifi_csi_provider_t provider;
  size_t max_sample_count;
} h2_pal_wifi_csi_capabilities_t;

/** Selects the associated AP and delivery interval for a CSI capture. */
typedef struct h2_pal_wifi_csi_config {
  uint8_t bssid[6];
  uint8_t bssid_set;
  uint32_t min_delivery_interval_ms;
} h2_pal_wifi_csi_config_t;

/**
 * A borrowed CSI frame delivered while a capture is active.
 *
 * The provider owns samples; a callback must copy them before it returns.
 */
typedef struct h2_pal_wifi_csi_frame {
  h2_pal_wifi_csi_provider_t provider;
  h2_pal_wifi_csi_phy_t phy;
  uint8_t channel;
  uint8_t bandwidth_mhz;
  uint8_t mcs;
  int8_t rssi_dbm;
  const h2_pal_wifi_csi_sample_t *samples;
  size_t sample_count;
} h2_pal_wifi_csi_frame_t;

/**
 * Receives one CSI frame.
 *
 * @param user Caller-owned context passed to h2_pal_wifi_csi_start.
 * @param frame Borrowed frame valid only for this callback invocation.
 *
 * Callbacks must not block or retain frame or frame->samples.
 */
typedef void (*h2_pal_wifi_csi_frame_fn)(void *user,
                                         const h2_pal_wifi_csi_frame_t *frame);

typedef struct h2_pal_wifi_csi_api h2_pal_wifi_csi_api_t;
typedef h2_pal_wifi_csi_api_t h2_pal_wifi_csi_t;

typedef h2_pal_result_t (*h2_pal_wifi_csi_get_capabilities_fn)(
    void *user, h2_pal_wifi_csi_capabilities_t *out_capabilities);
typedef h2_pal_result_t (*h2_pal_wifi_csi_start_fn)(
    void *user, const h2_pal_wifi_csi_config_t *config,
    h2_pal_wifi_csi_frame_fn frame_cb, void *frame_user);
typedef h2_pal_result_t (*h2_pal_wifi_csi_stop_fn)(void *user);

typedef struct h2_pal_wifi_csi_vtable {
  h2_pal_wifi_csi_get_capabilities_fn get_capabilities;
  h2_pal_wifi_csi_start_fn start;
  h2_pal_wifi_csi_stop_fn stop;
} h2_pal_wifi_csi_vtable_t;

struct h2_pal_wifi_csi_api {
  void *user;
  const h2_pal_wifi_csi_vtable_t *vtable;
};

/**
 * Reads provider capabilities.
 *
 * @param api CSI capability API.
 * @param out_capabilities Required output, cleared before a provider call.
 * @return H2_PAL_OK on success; otherwise an h2_pal_result_t error.
 */
static inline h2_pal_result_t h2_pal_wifi_csi_get_capabilities(
    const h2_pal_wifi_csi_api_t *api,
    h2_pal_wifi_csi_capabilities_t *out_capabilities) {
  if (api == NULL || api->vtable == NULL ||
      api->vtable->get_capabilities == NULL || out_capabilities == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_capabilities->provider = H2_PAL_WIFI_CSI_PROVIDER_UNKNOWN;
  out_capabilities->max_sample_count = 0u;
  return api->vtable->get_capabilities(api->user, out_capabilities);
}

/**
 * Starts delivery of CSI frames for the current STA association.
 *
 * @param api CSI capability API.
 * @param config Capture configuration; it is copied before this call returns.
 * @param frame_cb Required callback for borrowed frame delivery.
 * @param frame_user Caller-owned callback context.
 * @return H2_PAL_OK when capture starts; otherwise an h2_pal_result_t error.
 */
static inline h2_pal_result_t
h2_pal_wifi_csi_start(const h2_pal_wifi_csi_api_t *api,
                      const h2_pal_wifi_csi_config_t *config,
                      h2_pal_wifi_csi_frame_fn frame_cb, void *frame_user) {
  if (api == NULL || api->vtable == NULL || api->vtable->start == NULL ||
      config == NULL || frame_cb == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return api->vtable->start(api->user, config, frame_cb, frame_user);
}

/**
 * Stops CSI delivery and waits for any active frame callback to finish.
 *
 * @param api CSI capability API.
 * @return H2_PAL_OK on success; otherwise an h2_pal_result_t error.
 */
static inline h2_pal_result_t
h2_pal_wifi_csi_stop(const h2_pal_wifi_csi_api_t *api) {
  if (api == NULL || api->vtable == NULL || api->vtable->stop == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return api->vtable->stop(api->user);
}

#ifdef __cplusplus
}
#endif

#endif
