#ifndef H2_ALIPAY_SDK_H
#define H2_ALIPAY_SDK_H

#include "h2_alipay.h"
#include "os/h2_pal_mem.h"
#include "os/h2_pal_time.h"
#include "os/h2_pal_log.h"
#include "os/h2_pal_pref.h"
#include "os/h2_pal_sync.h"
#include "os/h2_pal_crypto.h"
#include "net/h2_pal_net.h"
#include "net/h2_pal_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Optional borrowed storage override. NULL selects the injected Pref PAL.
 * SDK indices and errors are passed unchanged. check_ready is retained for
 * source compatibility only and is not called by the SDK adapter. */
typedef struct h2_alipay_sdk_storage {
  void* user;
  int (*read)(void*, uint32_t, uint8_t*, uint32_t*);
  int (*write)(void*, uint32_t, const uint8_t*, uint32_t);
  int (*remove)(void*, uint32_t);
  int (*exists)(void*, uint32_t);
  h2_pal_result_t (*check_ready)(void*, uint32_t, h2_alipay_blocker_t*);
  int (*reset)(void*);
  int (*save_certificate)(void*, const void*, uint32_t);
} h2_alipay_sdk_storage_t;

typedef struct h2_alipay_sdk_config {
  const h2_pal_mem_api_t* mem;
  const h2_pal_time_api_t* time;
  const h2_pal_log_api_t* log;
  const h2_pal_sync_api_t* sync;
  const h2_pal_crypto_api_t* crypto;
  const h2_pal_net_api_t* net;
  const h2_pal_netif_api_t* netif;
  void* identity_user;
  h2_pal_result_t (*read_sn)(void*, char*, size_t);
  /** Product-selected SDK IMEI identity when has_modem is true. May return
   * the factory SN under an explicit SN substitution policy. Keep the value
   * stable across registration, binding and later signed requests. */
  h2_pal_result_t (*read_imei)(void*, char*, size_t);
  /** Borrowed device identity callback. Returns six MAC bytes on success. */
  h2_pal_result_t (*read_mac)(void*, uint8_t out[6]);
  bool has_modem;
  const char* company;
  const char* model;
  uint32_t product_type;
  const h2_alipay_sdk_storage_t* secure;
  /** Borrowed persistent Pref PAL, used when secure is NULL. */
  const h2_pal_pref_api_t* pref;
  /** Optional borrowed Pref namespaces, used when secure is NULL.
   * NULL preserves alipay_psa and alipay_cert, respectively. Select stable,
   * nonempty, distinct names for each identity policy; keep them until reset.
   * Changing identity must not reuse another identity's stored SDK context. */
  const char* psa_namespace;
  const char* certificate_namespace;
} h2_alipay_sdk_config_t;

/** Platform-independent original-SDK bridge. The supplied PAL APIs and
 * identity callbacks are borrowed until reset. Configure before any SDK or
 * vendor HAL call; callers serialize configuration itself. No teardown is
 * offered after SDK registration because this delivery cannot unregister.
 * Configure the process singleton once before any SDK operation; copies
 * config, borrows all pointers until reset. Duplicate configure is rejected.
 * The SDK's PSA callbacks use persistent Pref or the explicit override.
 * Binding-code requests follow the original SDK without a local readiness gate.
 * query_binding_result and binding_snapshot require default Pref storage
 * (secure == NULL). They expose only material lengths and status, verify four
 * committed SDK records, and keep a matching completion record in that same
 * PSA namespace. Pending status is not success even when SDK rc is zero.
 * After SDK completion, repeating the query only retries local verification;
 * it never repeats queryBindInfo/finishBind. Raw keys and seed stay in KV.
 * A local bound flag without the matching completion record requires explicit
 * verification rather than an automatic reset or a new binding. */
h2_pal_result_t h2_alipay_sdk_configure(const h2_alipay_sdk_config_t* config);
/** NULL until configured; never identifies a synthetic SDK as vendor. */
const h2_alipay_api_t* h2_alipay_sdk_api(void);

#ifdef __cplusplus
}
#endif
#endif
