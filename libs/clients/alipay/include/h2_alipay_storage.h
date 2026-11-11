#ifndef H2_ALIPAY_STORAGE_H
#define H2_ALIPAY_STORAGE_H

#include "os/h2_pal_pref.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Borrowed persistent Pref and allocator. Serialize operations through the
 * SDK caller. Each operation closes its namespace; successful mutations are
 * committed before returning. Results are zero or a negative PAL error.
 *
 * SDK indices use pXXXXXXXX keys in alipay_psa. reset clears only that
 * namespace. The separate alipay_cert/certificate record survives PSA reset,
 * as required by vendor_cert.h. These names are persistent storage identities.
 * Storage support does not add activation, enrollment or readiness policy. */
typedef struct h2_alipay_storage {
  const h2_pal_pref_api_t* pref;
  const h2_pal_mem_api_t* mem;
} h2_alipay_storage_t;

/** Read one complete SDK value. A short buffer is left unchanged and receives
 * the required length in size. Missing values return NOT_FOUND. */
int h2_alipay_storage_read(const h2_alipay_storage_t* storage, uint32_t index,
                           uint8_t* data, uint32_t* size);
int h2_alipay_storage_write(const h2_alipay_storage_t* storage, uint32_t index,
                            const uint8_t* data, uint32_t size);
int h2_alipay_storage_delete(const h2_alipay_storage_t* storage,
                             uint32_t index);
/** Zero means present, matching vendor_psa.h rather than C boolean semantics.
 */
int h2_alipay_storage_exists(const h2_alipay_storage_t* storage,
                             uint32_t index);
int h2_alipay_storage_reset(const h2_alipay_storage_t* storage);
int h2_alipay_storage_save_certificate(const h2_alipay_storage_t* storage,
                                       const void* data, uint32_t size);

#ifdef __cplusplus
}
#endif
#endif
