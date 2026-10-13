#include "h2_application_host_e2e.h"

h2_pal_result_t h2_application_host_e2e_run(h2_application_host_t *host,
                                            h2_runtime_t *runtime, void *user) {
    (void)user;
    h2_pal_result_t rc = h2_application_host_ready(host);
    while (rc == H2_PAL_OK && !h2_application_host_should_stop(host))
        rc = h2_pal_time_sleep_ms(runtime->time, 1u);
    return rc;
}
