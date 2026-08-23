#ifndef H2_GIZCLAW_E2E_H
#define H2_GIZCLAW_E2E_H

#include "h2_gizclaw_client.h"
#include "h2_gizclaw_registration.h"
#include "h2_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_E2E_REGISTRATION_TOKEN_MAX 4096u
#define H2_GIZCLAW_E2E_CASE_ID_CAPACITY 64u
#define H2_GIZCLAW_E2E_DEFAULT_CASE_TIMEOUT_MS 420000u
#define H2_GIZCLAW_E2E_DEFAULT_CLEANUP_TIMEOUT_MS 45000u
#define H2_GIZCLAW_E2E_DEFAULT_PROGRESS_INTERVAL_MS 10000u

typedef enum h2_gizclaw_e2e_suite {
  H2_GIZCLAW_E2E_SUITE_CONNECTIVITY = 1u << 0,
  H2_GIZCLAW_E2E_SUITE_RPC = 1u << 1,
  H2_GIZCLAW_E2E_SUITE_FIRMWARE = 1u << 2,
  H2_GIZCLAW_E2E_SUITE_VOICE = 1u << 3,
  H2_GIZCLAW_E2E_SUITE_CONCURRENCY = 1u << 4,
  H2_GIZCLAW_E2E_SUITE_SERVICE = 1u << 5,
  H2_GIZCLAW_E2E_SUITE_ALL = (1u << 6) - 1u,
} h2_gizclaw_e2e_suite_t;

typedef enum h2_gizclaw_e2e_exit {
  H2_GIZCLAW_E2E_EXIT_PASS = 0,
  H2_GIZCLAW_E2E_EXIT_CASE_FAILURE = 1,
  H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR = 2,
} h2_gizclaw_e2e_exit_t;

typedef enum h2_gizclaw_e2e_progress_kind {
  H2_GIZCLAW_E2E_PROGRESS_RUNNING = 0,
  H2_GIZCLAW_E2E_PROGRESS_CASE,
  H2_GIZCLAW_E2E_PROGRESS_CLEANUP,
  H2_GIZCLAW_E2E_PROGRESS_SUMMARY,
} h2_gizclaw_e2e_progress_kind_t;

typedef struct h2_gizclaw_e2e_progress {
  h2_gizclaw_e2e_progress_kind_t kind;
  const char *case_id;
  const char *status;
  const char *blocked_by;
  uint64_t elapsed_ms;
  size_t selected;
  size_t terminal;
  size_t passed;
  size_t failed;
  size_t errors;
  size_t blocked;
  size_t cancelled;
  int rc;
  bool complete;
} h2_gizclaw_e2e_progress_t;

typedef bool (*h2_gizclaw_e2e_should_stop_fn)(void *user);
/** Progress observers run synchronously on the calling task. */
typedef void (*h2_gizclaw_e2e_progress_fn)(
    void *user, const h2_gizclaw_e2e_progress_t *progress);

/**
 * All views are borrowed until h2_gizclaw_e2e_run() returns. The App copies
 * the RegistrationToken into App-owned bounded storage before registration and
 * erases that copy during cleanup. The token and user content are never passed
 * to the progress observer.
 */
typedef struct h2_gizclaw_e2e_config {
  h2_gizclaw_str_t server_endpoint;
  h2_gizclaw_str_t registration_token;
  const uint8_t *voice_pcm_s16le_16khz_mono;
  size_t voice_pcm_len;
  uint32_t suites;
  uint32_t case_timeout_ms;
  uint32_t cleanup_timeout_ms;
  uint32_t progress_interval_ms;
  h2_gizclaw_e2e_should_stop_fn should_stop;
  void *should_stop_user;
  h2_gizclaw_e2e_progress_fn on_progress;
  void *progress_user;
} h2_gizclaw_e2e_config_t;

typedef struct h2_gizclaw_e2e_result {
  size_t selected;
  size_t terminal;
  size_t passed;
  size_t failed;
  size_t errors;
  size_t blocked;
  size_t cancelled;
  char first_failure_case[H2_GIZCLAW_E2E_CASE_ID_CAPACITY];
  int first_failure_rc;
  int cleanup_rc;
  size_t retained_resources;
  char runtime_profile_name[H2_GIZCLAW_REGISTRATION_NAME_CAPACITY];
  bool complete;
} h2_gizclaw_e2e_result_t;

/**
 * Runs every selected independent case, performs bounded cleanup, and emits
 * one final summary. A normal return owns no live App resource. If a PAL
 * refuses to release the exited runner task before the cleanup deadline, the
 * result reports retained resources and the process-wide run guard remains held
 * so the retained PAL-owned context cannot race a later call. The calling task
 * emits progress while the App-owned runner executes cases; return occurs only
 * after that runner has exited, so retained handles cannot access borrowed
 * config or Runtime views. Concurrent calls are rejected with
 * H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR.
 */
h2_gizclaw_e2e_exit_t h2_gizclaw_e2e_run(h2_runtime_t *runtime,
                                         const h2_gizclaw_e2e_config_t *config,
                                         h2_gizclaw_e2e_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
