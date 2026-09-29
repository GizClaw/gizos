#ifndef H2_JSON_DEVICE_RUNNER_H
#define H2_JSON_DEVICE_RUNNER_H
#include "h2_pal_json_e2e.h"
/* Borrow runtime and a process-lifetime image version. Execute once per boot;
 * replay only emits the immutable completed ledger, never a second run. */
int h2_json_device_run(h2_runtime_t *runtime, const char *version);
void h2_json_device_replay(h2_runtime_t *runtime);
#endif
