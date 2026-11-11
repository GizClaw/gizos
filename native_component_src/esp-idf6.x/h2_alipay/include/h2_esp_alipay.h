#ifndef H2_ESP_ALIPAY_H
#define H2_ESP_ALIPAY_H

#include "h2_alipay.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2/pal/os/h2_pal_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Borrowed secure provider. It must implement the SDK PSA persistence
 * contract for the selected ECDH delivery, including protected storage,
 * credential retention and independent readback. No RAM/default provider.
 * SDK indices and errors are passed unchanged. reset is intentionally absent:
 * this stage cannot erase credentials or account data. */
typedef struct h2_esp_alipay_secure {
  void* user;
  int (*read)(void*, uint32_t, uint8_t*, uint32_t*);
  int (*write)(void*, uint32_t, const uint8_t*, uint32_t);
  int (*remove)(void*, uint32_t);
  int (*exists)(void*, uint32_t);
  h2_pal_result_t (*check_ready)(void*, uint32_t, h2_alipay_blocker_t*);
} h2_esp_alipay_secure_t;

typedef struct h2_esp_alipay_config {
  const h2_pal_mem_api_t* mem;
  const h2_pal_time_api_t* time;
  const h2_pal_log_api_t* log;
  void* identity_user;
  h2_pal_result_t (*read_sn)(void*, char*, size_t);
  /** Required when a modem is fitted; no transport-dependent SN fallback. */
  h2_pal_result_t (*read_imei)(void*, char*, size_t);
  bool has_modem;
  const char* company;
  const char* model;
  uint32_t product_type;
  const h2_esp_alipay_secure_t* secure;
} h2_esp_alipay_config_t;

/** Configure the process singleton once before any SDK operation; copies
 * config, borrows all pointers until reset. Duplicate configure is rejected.
 * NULL secure permits pre_init only; code requests report a specific blocker.
 * No secret, activation or provisioning is inferred from product strings. */
h2_pal_result_t h2_esp_alipay_configure(const h2_esp_alipay_config_t* config);
/** NULL until configured; never identifies a synthetic SDK as vendor. */
const h2_alipay_api_t* h2_esp_alipay_api(void);

#ifdef __cplusplus
}
#endif
#endif
