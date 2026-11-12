#include "h2_alipay_tiga_protocol.h"

#include "h2_alipay_e2e_independent.h"
#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_identity.h"
#include "esp_mac.h"
#include <stdio.h>
#include <string.h>

static h2_runtime_t* s_runtime;
static h2_alipay_client_config_t s_config;
static char s_sn[49], s_mac[18];

h2_pal_result_t h2_alipay_tiga_protocol_configure(
    h2_runtime_t* runtime, uint32_t compile_timestamp_s) {
  if (runtime == NULL || compile_timestamp_s == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (s_runtime != NULL) return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_alipay_tiga_read_sn(runtime, s_sn, sizeof(s_sn));
  uint8_t mac[6];
  if (rc == H2_PAL_OK)
    rc = esp_read_mac(mac, ESP_MAC_BT) == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
  if (rc != H2_PAL_OK) return rc;
  (void)snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
                 (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
  s_config = (h2_alipay_client_config_t){
      .mem = runtime->mem,
      .time = runtime->time,
      .http = runtime->http,
      .pref = runtime->pref,
      .crypto = runtime->crypto,
      .identity = {.sn = s_sn, .imei = s_sn, .mac = s_mac, .model = "H-106TG"},
      .company = "HAIVIVI",
      .pref_namespace = "alipay_direct",
      .feature = 16385u,
      .compile_timestamp_s = compile_timestamp_s,
  };
  rc = h2_alipay_e2e_independent_configure(&s_config, runtime->sync);
  if (rc != H2_PAL_OK) return rc;
  s_runtime = runtime;
  return H2_PAL_OK;
}

const h2_alipay_api_t* h2_alipay_e2e_target_api(void) {
  return h2_alipay_e2e_independent_api();
}
