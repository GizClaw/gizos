#ifndef H2_ALIPAY_TIGA_PROTOCOL_H
#define H2_ALIPAY_TIGA_PROTOCOL_H

#include "h2_alipay.h"
#include "h2_runtime.h"

/** Configure the resident independent client. An optional borrowed SDK API
 * supplies a reference run before the independent request; it is never used
 * by the independent library. Dependencies remain alive until reset. */
h2_pal_result_t h2_alipay_tiga_protocol_configure(
    h2_runtime_t* runtime, const h2_alipay_api_t* reference);

#endif
