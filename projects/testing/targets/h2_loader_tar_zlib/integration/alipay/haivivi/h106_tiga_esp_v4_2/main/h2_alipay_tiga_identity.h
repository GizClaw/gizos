#ifndef H2_ALIPAY_TIGA_IDENTITY_H
#define H2_ALIPAY_TIGA_IDENTITY_H

#include "h2_runtime.h"

h2_pal_result_t h2_alipay_tiga_read_sn(void* user, char* out, size_t capacity);
/** Blocking SDK-worker callback. Idempotently opens the shared Modem PAL
 * before reading its actual identity. The final entry retains the modem for
 * the SDK's boot lifetime; individual identity reads do not close it. */
h2_pal_result_t h2_alipay_tiga_read_imei(void* user, char* out,
                                         size_t capacity);

#endif
