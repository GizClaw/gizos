#ifndef H2_ALIPAY_TIGA_STORAGE_H
#define H2_ALIPAY_TIGA_STORAGE_H

#include "h2_runtime.h"
#include "h2_alipay.h"

/** Boot-lifetime storage assembly. Session mode purges only the explicitly
 * listed historical Alipay test namespaces, then routes the SDK PSA and
 * independent context namespaces to static variables owned by this entry.
 * Those namespaces support typed blob/U32 get/set, remove, clear, commit
 * and close. Reading a value through the wrong type returns FORMAT.
 * Mutations apply at once; values are bounded, allocated through Runtime
 * Memory, wiped on replacement and discarded by hardware reset. Callers
 * serialize operations. One reader and one writer may be open per session
 * namespace; a duplicate open returns BUSY. Factory identity and certificate
 * namespaces stay on Runtime Pref. Repeated calls for the same Runtime reuse
 * the instance. */
h2_pal_result_t h2_alipay_tiga_storage_configure(h2_runtime_t* runtime,
                                                 const h2_pal_pref_api_t** out);
h2_alipay_storage_scope_t h2_alipay_tiga_storage_scope(void);

/** Quiescent-only metadata audit: verifies legacy persistent test namespaces
 * remain empty and reports current session entry count, without payloads. */
h2_pal_result_t h2_alipay_tiga_storage_audit(void);

/** Quiescent-only release for host composition tests; wipes session values
 * and returns INVALID_STATE while a session namespace is open. Never call
 * after the original SDK has registered callbacks borrowing this provider. */
h2_pal_result_t h2_alipay_tiga_storage_release(void);
#endif
