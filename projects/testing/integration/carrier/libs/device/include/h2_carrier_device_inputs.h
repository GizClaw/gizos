#ifndef H2_CARRIER_DEVICE_INPUTS_H
#define H2_CARRIER_DEVICE_INPUTS_H

#include "h2_lingxi.h"
#include "h2_onenet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_CARRIER_DEVICE_CONFIG_BYTES 16384u

/** Caller-owned runtime provisioning, never a firmware build input. The
 * launcher supplies a calibrated epoch and an authorized identity for each
 * platform. Device metadata comes from the executing Runtime. */
typedef struct h2_carrier_device_inputs {
  uint64_t epoch_ms;
  bool has_lingxi;
  bool has_onenet;
  char lingxi_base[H2_LINGXI_BASE_URL_CAPACITY];
  char lingxi_product[H2_LINGXI_ID_CAPACITY];
  char lingxi_key[H2_LINGXI_PRODUCT_KEY_CAPACITY];
  char device_no[H2_LINGXI_DEVICE_NO_CAPACITY];
  h2_lingxi_device_no_type_t device_no_type;
  char onenet_host[H2_ONENET_HOST_CAPACITY];
  char onenet_tls_server_name[H2_ONENET_HOST_CAPACITY];
  char onenet_ca_pem[8192];
  uint16_t onenet_port;
  char onenet_product[H2_ONENET_NAME_CAPACITY];
  char onenet_device[H2_ONENET_NAME_CAPACITY];
  char onenet_token[H2_ONENET_TOKEN_CAPACITY];
  char onenet_device_key[4u * ((H2_ONENET_KEY_MAX_BYTES + 2u) / 3u) + 1u];
  char onenet_params[4096];
} h2_carrier_device_inputs_t;

/** Parse bounded JSON using a borrowed provider. Inputs must not alias the
 * output, which is cleared before validation and on every failure. No I/O,
 * credential logging or provider lifecycle is owned here. */
h2_pal_result_t h2_carrier_device_inputs_parse(const h2_pal_json_api_t* json,
                                               const uint8_t* bytes,
                                               size_t length,
                                               h2_carrier_device_inputs_t* out);
/** Volatile wipe of caller-owned inputs; NULL is a no-op. */
void h2_carrier_device_inputs_clear(h2_carrier_device_inputs_t* inputs);

#ifdef __cplusplus
}
#endif
#endif
