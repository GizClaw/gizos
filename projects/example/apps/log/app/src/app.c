#include "h2_log_example.h"

int h2_log_example_run(h2_runtime_t *runtime) {
  if (runtime == NULL || runtime->log == NULL ||
      runtime->log->vtable == NULL || runtime->log->vtable->write == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "log",
                          "Hello World");
}
