#ifndef H2_STORAGE_DEVICE_RUNNER_H
#define H2_STORAGE_DEVICE_RUNNER_H
#include "h2_pal_storage_e2e.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Called once per boot. This fixture owns h2storectl, h2storea, h2storeb and
 * the supplied test directory. Stage is tied to the actual image version. */
int h2_storage_device_run(h2_runtime_t *runtime,const char *directory,const char *version);
void h2_storage_device_replay(h2_runtime_t *runtime);

#define H2_STORAGE_DEVICE_RECORD_MAX 512u
/** Storage-only synchronous output. Record bytes are borrowed only during
 * this call; length includes the newline and excludes the NUL terminator.
 * Success means the whole record was delivered and drained. */
typedef int (*h2_storage_device_output_fn)(void *user,const char *record,size_t length);
/** @brief Run once per boot with checked record delivery.
 * Borrow runtime, version and output/user through every replay; directory is
 * borrowed only during this call. A non-NULL callback is required.
 * The first delivery error remains sticky; failures before phase-control
 * commit prevent that commit, while already completed commits are not undone.
 * Return suite, control or delivery failure; no App confirmation is performed.
 */
int h2_storage_device_run_with_output(h2_runtime_t *runtime,const char *directory,
    const char *version,h2_storage_device_output_fn output,void *user);
/** @brief Deliver the frozen current-boot ledger without running new cases.
 * Call after run, with the same borrowed output/user still alive. Return the
 * delivery result; suite/control results remain in the terminal record.
 */
int h2_storage_device_replay_checked(h2_runtime_t *runtime);
#ifdef __cplusplus
}
#endif
#endif
