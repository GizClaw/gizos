#ifndef H2_ATOMIC_DEVICE_RUNNER_H
#define H2_ATOMIC_DEVICE_RUNNER_H
#include "h2_atomic_e2e.h"
#include "h2_runtime.h"
int h2_atomic_device_run(h2_runtime_t *runtime, const char *version,
                         h2_atomic_qualification_config_t *config,
                         const h2_pal_mem_api_t *external);
void h2_atomic_device_replay(void);
#endif
