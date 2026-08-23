#ifndef H2_LOG_EXAMPLE_H
#define H2_LOG_EXAMPLE_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Emit one Hello World record through the borrowed Runtime Log service.
 *
 * The Runtime must remain valid for the complete blocking call. This function
 * does not initialize, drain, or release the borrowed Log provider.
 *
 * @param runtime Borrowed initialized Runtime providing a usable Log service.
 * @return H2_PAL_OK on success, otherwise an argument or Log provider error.
 */
int h2_log_example_run(h2_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
