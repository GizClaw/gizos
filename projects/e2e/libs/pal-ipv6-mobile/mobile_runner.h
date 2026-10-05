#ifndef H2_IPV6_MOBILE_RUNNER_H
#define H2_IPV6_MOBILE_RUNNER_H
#include "h2_pal_ipv6_runner.h"
int h2_ipv6_mobile_run(h2_runtime_config_t, const char *, uint16_t,
                       const char *, const uint8_t *, size_t, const uint8_t *,
                       size_t, const char *, const char *, const char *,
                       const char *, uint16_t, const char *, const char *,
                       uint16_t, h2_pal_ipv6_result_t *);
#endif
