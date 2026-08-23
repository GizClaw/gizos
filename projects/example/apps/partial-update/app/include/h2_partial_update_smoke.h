#ifndef H2_PARTIAL_UPDATE_SMOKE_H
#define H2_PARTIAL_UPDATE_SMOKE_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_partial_update_smoke_config {
    const char *app_generation;
} h2_partial_update_smoke_config_t;

int h2_partial_update_smoke_run(
    h2_runtime_t *runtime,
    const h2_partial_update_smoke_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
