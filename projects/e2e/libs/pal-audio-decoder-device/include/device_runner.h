#ifndef H2_ADEC_DEVICE_RUNNER_H
#define H2_ADEC_DEVICE_RUNNER_H
#include "h2_runtime.h"
/* Run once per boot; replay the completed immutable ledger for UART readers. */
int h2_adec_device_run(h2_runtime_t *runtime, const char *version);
void h2_adec_device_replay(h2_runtime_t *runtime);
#endif
