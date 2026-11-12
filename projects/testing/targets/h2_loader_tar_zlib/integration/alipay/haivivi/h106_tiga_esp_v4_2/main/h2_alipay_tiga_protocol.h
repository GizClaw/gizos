#ifndef H2_ALIPAY_TIGA_PROTOCOL_H
#define H2_ALIPAY_TIGA_PROTOCOL_H

#include "h2_alipay.h"
#include "h2_runtime.h"

/** Configure the resident independent original-API implementation.
 * Dependencies remain alive until reset. No vendor backend is linked. */
h2_pal_result_t h2_alipay_tiga_protocol_configure(h2_runtime_t* runtime,
                                                  uint32_t compile_timestamp_s);

#endif
