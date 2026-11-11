#ifndef H2_ALIPAY_TIGA_GATEWAY_H
#define H2_ALIPAY_TIGA_GATEWAY_H
#include "h2_runtime.h"
/* Worker-side DNS/TCP/verified TLS/HTTP probe. Sends no SDK credentials. */
h2_pal_result_t h2_alipay_tiga_gateway_probe(h2_runtime_t* runtime);
#endif
