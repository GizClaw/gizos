#ifndef H2_PAL_IPV6_DEVICE_H
#define H2_PAL_IPV6_DEVICE_H
#include "h2_pal_ipv6_e2e.h"

int h2_pal_ipv6_device_board_fixture(void);
const char *h2_pal_ipv6_device_host(void);
extern const char h2_pal_ipv6_device_task_name[];
int h2_pal_ipv6_device_run(h2_runtime_t *, const h2_pal_dtls_api_t *,
                           h2_pal_ipv6_result_t *);
void h2_pal_ipv6_device_report(const h2_runtime_t *,
                               const h2_pal_ipv6_result_t *);
#endif
