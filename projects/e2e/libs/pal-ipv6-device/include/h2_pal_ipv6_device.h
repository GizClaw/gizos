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
/** Run once per boot with an optional borrowed result Log, without changing
 * Runtime or provider logging. The sink and user must remain valid through run,
 * report and retained cleanup, serialize writes, and preserve the first error.
 * NULL keeps the legacy Runtime Log. No App confirmation occurs here. */
int h2_pal_ipv6_device_run_with_report_log(h2_runtime_t *,
    const h2_pal_dtls_api_t *, const h2_pal_log_api_t *, h2_pal_ipv6_result_t *);
/** Deliver all 56 frozen case records and the same boot's summary. Return the
 * first formatting, Log or pacing error; the caller must not confirm on error.
 * This never reruns a case or changes the Runtime Log. NULL uses Runtime Log. */
int h2_pal_ipv6_device_report_with_log(const h2_runtime_t *,
    const h2_pal_log_api_t *, const h2_pal_ipv6_result_t *);
#endif
