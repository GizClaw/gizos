#ifndef H2_GIZCLAW_E2E_EXECUTION_H
#define H2_GIZCLAW_E2E_EXECUTION_H

#include "h2_gizclaw_e2e.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_e2e_execution h2_gizclaw_e2e_execution_t;

/** Evidence metadata and an already prepared, borrowed output capability.
 * The output Log PAL must preserve each complete newline-terminated record
 * verbatim, without prefixes or truncation (records are at most 1056 bytes).
 * The App serializes its output calls, including concurrent diagnostics. The
 * target owns console arbitration with other users and the output's lifetime.
 * Identity strings are non-secret marker fields, never provider selectors;
 * each is 1-63 ASCII letters, digits, dots, underscores or hyphens. */
typedef struct h2_gizclaw_e2e_execution_config {
  const h2_pal_log_api_t* output;
  const char* board;
  const char* platform;
  bool physical_audio;
} h2_gizclaw_e2e_execution_config_t;

/** Allocate the bounded capture context through runtime.mem. Runtime and all
 * config views are borrowed; the App copies config and installs its own
 * evidence observer. config must select SUITE_ALL and have no evidence
 * observer. No providers, networking, trust policy or Runtime lifetime are
 * selected here. All API calls on an execution are caller-serialized
 * task-context operations. On failure, a non-NULL output still owns resources
 * if cleanup failed; keep its dependencies alive and retry destroy or retain it
 * through process exit. */
int h2_gizclaw_e2e_execution_create(
    h2_runtime_t* runtime, const h2_gizclaw_e2e_config_t* config,
    const h2_gizclaw_e2e_execution_config_t* evidence,
    h2_gizclaw_e2e_execution_t** out_execution);

/** Emit BOOT, run once, capture the final summary, then freeze only after a
 * zero-retention return with successful capture. A second run is rejected.
 * Retained resources keep the observer, buffers, config and Runtime borrowed
 * until process exit, even after this function returns. */
h2_gizclaw_e2e_exit_t h2_gizclaw_e2e_execution_run(
    h2_gizclaw_e2e_execution_t* execution);

/** True only after run returned PASS with the complete full-suite result,
 * no retained resources, successful capture and an immutable frozen ledger.
 * The target may confirm its image only after this gate succeeds. */
bool h2_gizclaw_e2e_execution_qualified(
    const h2_gizclaw_e2e_execution_t* execution);

/** After the target's confirmation decision, emit summary then READY once.
 * Pass INVALID_STATE when confirmation was not attempted. A successful confirm
 * result is rejected unless the App qualified; confirmation itself remains
 * entirely target-owned. Returns an output error without resuming a record. */
int h2_gizclaw_e2e_execution_publish(h2_gizclaw_e2e_execution_t* execution,
                                     int confirm_rc);

/** Replay one frozen ledger as begin, complete records, end. Unfrozen evidence
 * is never replayed. No retry, sleep or periodic work is performed here; the
 * target owns the replay interval. A failed output aborts this replay. */
int h2_gizclaw_e2e_execution_replay(h2_gizclaw_e2e_execution_t* execution);

/** Release a quiescent execution and set *execution to NULL. Refuses running
 * or retained resources. On failed mutex destruction, retain the context and
 * its borrowed dependencies for retry. NULL *execution succeeds. */
int h2_gizclaw_e2e_execution_destroy(h2_gizclaw_e2e_execution_t** execution);

#ifdef __cplusplus
}
#endif

#endif
