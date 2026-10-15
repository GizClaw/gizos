#ifndef H2_CRYPTO_DEVICE_RUNNER_H
#define H2_CRYPTO_DEVICE_RUNNER_H
#include "h2_pal_crypto_e2e.h"
/* Borrow runtime and a process-lifetime image version. Execute once per boot;
 * replay only emits the immutable completed ledger, never a second run. */
int h2_crypto_device_run(h2_runtime_t *runtime, const char *version);
void h2_crypto_device_replay(h2_runtime_t *runtime);
#endif
