#ifndef H2_ATOMIC_MOBILE_RUNNER_H
#define H2_ATOMIC_MOBILE_RUNNER_H
#include "h2_runtime.h"
int h2_atomic_mobile_run(h2_runtime_config_t config, const char *platform,
                         const char *version, const char *path,
                         int (*shutdown)(void));
#endif
