#ifndef H2_CRYPTO_MOBILE_RUNNER_H
#define H2_CRYPTO_MOBILE_RUNNER_H
#include "h2_runtime.h"
/* Borrow config APIs for one synchronous run, then retire Runtime before
 * invoking the supplied platform shutdown. The report path is caller-owned. */
int h2_crypto_mobile_run(h2_runtime_config_t config, const char *platform,
                         const char *version, const char *path,
                         int (*shutdown)(void));
#endif
