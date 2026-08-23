#ifndef H2_MODEM_SMOKE_H
#define H2_MODEM_SMOKE_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_modem_smoke_config {
    const char *apn;
    uint32_t registration_timeout_ms;
    uint32_t data_timeout_ms;
    uint32_t ping_timeout_ms;
} h2_modem_smoke_config_t;

h2_pal_result_t h2_modem_smoke_run(
    h2_runtime_t *runtime,
    const h2_modem_smoke_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
