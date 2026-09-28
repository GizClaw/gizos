#ifndef H2_STORAGE_DEVICE_RUNNER_H
#define H2_STORAGE_DEVICE_RUNNER_H
#include "h2_pal_storage_e2e.h"
/* Called once per boot. This fixture owns h2storectl, h2storea, h2storeb and
 * the supplied test directory. Stage is tied to the actual image version. */
int h2_storage_device_run(h2_runtime_t *runtime,const char *directory,const char *version);
void h2_storage_device_replay(h2_runtime_t *runtime);
#endif
