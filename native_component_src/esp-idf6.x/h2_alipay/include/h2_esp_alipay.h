#ifndef H2_ESP_ALIPAY_H
#define H2_ESP_ALIPAY_H

#include "h2_alipay.h"
#include "os/h2_pal_mem.h"
#include "os/h2_pal_time.h"
#include "os/h2_pal_log.h"
#include "os/h2_pal_pref.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Optional borrowed storage override. NULL selects the injected Pref PAL.
 * SDK indices and errors are passed unchanged. check_ready is retained for
 * source compatibility only and is not called by the SDK adapter. */
typedef struct h2_esp_alipay_secure {
  void* user;
  int (*read)(void*, uint32_t, uint8_t*, uint32_t*);
  int (*write)(void*, uint32_t, const uint8_t*, uint32_t);
  int (*remove)(void*, uint32_t);
  int (*exists)(void*, uint32_t);
  h2_pal_result_t (*check_ready)(void*, uint32_t, h2_alipay_blocker_t*);
  int (*reset)(void*);
  int (*save_certificate)(void*, const void*, uint32_t);
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
  /** Borrowed persistent Pref PAL, used when secure is NULL. */
  const h2_pal_pref_api_t* pref;
} h2_esp_alipay_config_t;

/** Configure the process singleton once before any SDK operation; copies
 * config, borrows all pointers until reset. Duplicate configure is rejected.
 * The SDK's PSA callbacks use persistent Pref or the explicit override.
 * Binding calls follow the original SDK without a local readiness gate. */
h2_pal_result_t h2_esp_alipay_configure(const h2_esp_alipay_config_t* config);
/** NULL until configured; never identifies a synthetic SDK as vendor. */
const h2_alipay_api_t* h2_esp_alipay_api(void);

#ifdef __cplusplus
}
#endif
#endif
