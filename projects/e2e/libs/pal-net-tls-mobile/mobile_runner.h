#ifndef H2_NET_TLS_MOBILE_H
#define H2_NET_TLS_MOBILE_H
#include "runner.h"
int h2_net_tls_mobile_run(h2_runtime_config_t, const char *, uint16_t,
                          const char *, const uint8_t *, size_t,
                          const uint8_t *, size_t, const char *, const char *,
                          h2_net_tls_result_t *);
#endif
