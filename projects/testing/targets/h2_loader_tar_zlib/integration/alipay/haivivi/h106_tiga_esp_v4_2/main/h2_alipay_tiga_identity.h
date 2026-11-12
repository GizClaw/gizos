#ifndef H2_ALIPAY_TIGA_IDENTITY_H
#define H2_ALIPAY_TIGA_IDENTITY_H

#include "h2_runtime.h"

h2_pal_result_t h2_alipay_tiga_read_sn(void* user, char* out, size_t capacity);
/** SDK-worker callback returning the same persistent factory SN as read_sn.
 * This entry always uses SN for the SDK IMEI identity, including later RPC
 * signatures. Factory read errors propagate; the callback never uses Modem. */
h2_pal_result_t h2_alipay_tiga_read_imei(void* user, char* out,
                                         size_t capacity);

#endif
