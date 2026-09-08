#ifndef H2_GIZCLAW_SESSION_H
#define H2_GIZCLAW_SESSION_H

#include "h2_gizclaw_conversation.h"
#include "h2_gizclaw_registration.h"
#include "h2_gizclaw_workflow.h"
#include "h2_gizclaw_workspace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_session h2_gizclaw_session_t;

typedef enum h2_gizclaw_session_phase {
  H2_GIZCLAW_SESSION_EMPTY = 0,
  H2_GIZCLAW_SESSION_PREPARING,
  H2_GIZCLAW_SESSION_READY,
  H2_GIZCLAW_SESSION_FAILED,
  H2_GIZCLAW_SESSION_CLOSED,
} h2_gizclaw_session_phase_t;

typedef enum h2_gizclaw_session_blocker {
  H2_GIZCLAW_SESSION_BLOCK_NONE = 0,
  H2_GIZCLAW_SESSION_BLOCK_REGISTRATION,
  H2_GIZCLAW_SESSION_BLOCK_CATALOG,
  H2_GIZCLAW_SESSION_BLOCK_WORKSPACE,
  H2_GIZCLAW_SESSION_BLOCK_CONVERSATION,
  H2_GIZCLAW_SESSION_BLOCK_CLOSED,
} h2_gizclaw_session_blocker_t;

typedef enum h2_gizclaw_session_conversation_phase {
  H2_GIZCLAW_SESSION_CONVERSATION_IDLE = 0,
  H2_GIZCLAW_SESSION_CONVERSATION_RECORDING,
  H2_GIZCLAW_SESSION_CONVERSATION_WAITING,
  H2_GIZCLAW_SESSION_CONVERSATION_REPLYING,
  H2_GIZCLAW_SESSION_CONVERSATION_CALLING,
} h2_gizclaw_session_conversation_phase_t;

/** Copied, pointer-free state. READY describes server-confirmed facts, not
 * device microphone readiness. revision changes on every published transition.
 */
typedef struct h2_gizclaw_session_state {
  uint64_t revision;
  uint64_t generation;
  h2_gizclaw_session_phase_t registration;
  h2_gizclaw_session_phase_t catalog;
  h2_gizclaw_session_phase_t workspace;
  h2_gizclaw_session_conversation_phase_t conversation;
  bool conversation_input_open;
  /** Confirmed workspace parameters. Unset patch members preserve these values.
   * input distinguishes PTT (IDLE/RECORDING/WAITING/REPLYING) from realtime
   * (IDLE/CALLING). Errors are results, never conversation phases. */
  h2_gizclaw_workspace_parameters_patch_t parameters;
  char profile_name[H2_GIZCLAW_REGISTRATION_NAME_CAPACITY];
  char profile_revision[H2_GIZCLAW_REGISTRATION_NAME_CAPACITY];
  char current_workspace[H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES + 1u];
  char target_workspace[H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES + 1u];
  char workflow_name[H2_GIZCLAW_WORKFLOW_NAME_MAX_BYTES + 1u];
  size_t workflow_count;
  bool can_start;
  h2_gizclaw_session_blocker_t blocking_reason;
  h2_gizclaw_session_blocker_t error_stage;
  h2_pal_result_t last_error;
  /** Original remote conversation error; empty for local failures or success. */
  char error_code[65];
  bool retryable;
} h2_gizclaw_session_state_t;

/** Dependencies and collection strings are borrowed until destroy. One Session
 * per Service; use Session operations exclusively for registration, catalog,
 * and conversations on that Service. Existing synchronous workspace RPCs
 * participate in this Session's lifecycle and publish confirmed parameters.
 * Low-level asynchronous workspace requests must not bypass this owner. No
 * product names, defaults or persistence paths are built in.
 */
typedef struct h2_gizclaw_session_config {
  h2_gizclaw_service_t *service;
  const h2_pal_mem_api_t *mem;
  const h2_pal_sync_api_t *sync;
  const h2_pal_time_api_t *time;
  h2_runtime_t *runtime;
  const char *const *collections;
  size_t collection_count;
  size_t max_workflows;
  size_t catalog_bytes;
} h2_gizclaw_session_config_t;

/** Product-selected names; NULL collection/workflow opens an existing workspace
 * without creating it. parameters == NULL preserves server input policy. */
typedef struct h2_gizclaw_session_selection {
  const char *collection;
  const char *workflow_name;
  const char *workspace_name;
  const h2_gizclaw_workspace_parameters_patch_t *parameters;
} h2_gizclaw_session_selection_t;

h2_pal_result_t
h2_gizclaw_session_create(const h2_gizclaw_session_config_t *config,
                          h2_gizclaw_session_t **out_session);
/** Requires all Session operations joined and conversation handles released.
 * Returns BUSY while owned operations remain. Does not destroy the Service. */
h2_pal_result_t h2_gizclaw_session_destroy(h2_gizclaw_session_t **session);
/** Thread-safe copied reads; never perform RPC or expose mutable library
 * memory. Changes notify the configured Runtime; observers re-read the
 * snapshot. */
h2_pal_result_t
h2_gizclaw_session_snapshot(h2_gizclaw_session_t *session,
                            h2_gizclaw_session_state_t *out_state);
/** Copies the complete valid catalog and Profile identity into caller storage.
 * Failure clears out_catalog. Retained stale data is never returned as valid.
 */
h2_pal_result_t
h2_gizclaw_session_catalog_copy(h2_gizclaw_session_t *session,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_workflow_page_t *out_catalog);
/** Blocking operations belong on a caller task, never service_poll callbacks or
 * the Service worker. Preparations are serialized. Select/conversation requests
 * wait behind preparation within their deadline; register/refresh return BUSY.
 * timeout_ms is a total monotonic deadline, including every page and RPC.
 * Registration automatically loads the requested catalog. Catalog failure does
 * not undo successful registration; inspect both phases in the snapshot. */
h2_pal_result_t h2_gizclaw_session_register(h2_gizclaw_session_t *session,
                                            const char *token,
                                            uint32_t timeout_ms);
h2_pal_result_t h2_gizclaw_session_refresh(h2_gizclaw_session_t *session,
                                           uint32_t timeout_ms);
/** Ensures catalog validity and prepares the named Workspace. Current changes
 * only after server confirmation; a failed switch cannot publish its target as
 * current. A version mismatch permits one catalog refresh and retry. */
h2_pal_result_t
h2_gizclaw_session_select(h2_gizclaw_session_t *session,
                          const h2_gizclaw_session_selection_t *selection,
                          uint32_t timeout_ms);
/** Close admission immediately and discard late preparation results. Call when
 * Service becomes terminal or before stopping it; Service stop interrupts RPC.
 * A Session is connection-scoped and cannot be reopened. */
h2_pal_result_t h2_gizclaw_session_close(h2_gizclaw_session_t *session);
/** Prepare selection then configure the conversation route. Does not start the
 * microphone: service_audio_start remains the explicit audio boundary. */
h2_pal_result_t h2_gizclaw_session_conversation_create(
    h2_gizclaw_session_t *session,
    const h2_gizclaw_session_selection_t *selection, uint32_t timeout_ms,
    h2_gizclaw_conversation_callback_fn callback,
    h2_gizclaw_conversation_completion_fn completion, void *user,
    h2_gizclaw_conversation_t **out_conversation);
/** Start/end input on the Session-owned conversation route and update the
 * public input state. Completion still comes through service_poll. Start
 * interrupts a previous waiting/replying generation on the same route, waiting
 * up to 30 seconds for local cancellation dispatch (not for the agent reply).
 * Repeated start while recording and end while idle are harmless. Call from a
 * control task; service_poll must continue on its owner while start waits.
 */
h2_pal_result_t h2_gizclaw_session_audio_start(h2_gizclaw_session_t *session);
h2_pal_result_t h2_gizclaw_session_audio_end(h2_gizclaw_session_t *session);
/** Discard an in-flight preparation result without closing the connection.
 * The currently executing RPC remains bounded by its deadline; subsequent
 * steps are skipped. Does not cancel an already-created conversation. */
h2_pal_result_t
h2_gizclaw_session_cancel_pending(h2_gizclaw_session_t *session);
/** Release only after idle/terminal, as required by Conversation. Use this
 * instead of raw conversation_release for Session-created conversations. */
void h2_gizclaw_session_conversation_release(
    h2_gizclaw_session_t *session, h2_gizclaw_conversation_t *conversation);

#ifdef __cplusplus
}
#endif
#endif
