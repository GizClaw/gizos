#ifndef H2_ALIPAY_TIGA_IDENTITY_H
#define H2_ALIPAY_TIGA_IDENTITY_H

#include "h2_runtime.h"

h2_pal_result_t h2_alipay_tiga_read_sn(void* user, char* out, size_t capacity);
h2_pal_result_t h2_alipay_tiga_read_imei(void* user, char* out,
                                         size_t capacity);

#endif
