#ifndef H2_ALIPAY_TIGA_PROTOCOL_H
#define H2_ALIPAY_TIGA_PROTOCOL_H

#include "h2_alipay.h"
#include "h2_runtime.h"

/** Configure the resident independent original-API implementation.
 * Selects the portable wolfCrypt provider using Runtime Crypto PAL entropy.
 * An already selected wolfCrypt provider is borrowed; otherwise this entry
 * owns its initialization until release. Dependencies remain borrowed until
 * release or reset. Callers serialize configure/release with every SDK call.
 * No vendor backend is linked. */
h2_pal_result_t h2_alipay_tiga_protocol_configure(h2_runtime_t* runtime,
                                                  uint32_t compile_timestamp_s);

/** After all workers join, release the facade then an entry-owned provider.
 * Failure retains dependencies for retry. Repeated release is OK. The caller
 * separately releases target storage before destroying Runtime dependencies.
 */
h2_pal_result_t h2_alipay_tiga_protocol_release(void);

#endif
