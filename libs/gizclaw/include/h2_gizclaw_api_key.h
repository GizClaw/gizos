#ifndef H2_GIZCLAW_API_KEY_H
#define H2_GIZCLAW_API_KEY_H
#include "h2_gizclaw_service.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Caller-owned secret. Never log; erase when no longer required. */
typedef struct h2_gizclaw_api_key {
  char name[27];
  char secret[96];
} h2_gizclaw_api_key_t;
h2_pal_result_t h2_gizclaw_req_create_api_key_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t display_name, bool manage_api_keys, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_api_key_create(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_api_key_t *out_key);
h2_pal_result_t h2_gizclaw_rpc_api_key_create(h2_gizclaw_service_t *service,
                                              h2_gizclaw_str_t display_name,
                                              bool manage_api_keys,
                                              uint32_t timeout_ms,
                                              h2_gizclaw_api_key_t *out_key);
h2_pal_result_t h2_gizclaw_req_create_api_key_revoke(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_api_key_revoke(const h2_gizclaw_req_t *request);
h2_pal_result_t h2_gizclaw_rpc_api_key_revoke(h2_gizclaw_service_t *service,
                                              h2_gizclaw_str_t name,
                                              uint32_t timeout_ms);
/** Completion-driven secret state, independent of the blocking resource store.
 * request_refresh/close and service_poll belong to the same owner task.
 * snapshot also supports other threads; destroy requires exclusive access.
 */
typedef struct h2_gizclaw_api_key_state h2_gizclaw_api_key_state_t;

/** Required dependencies are borrowed until state destroy. */
typedef struct h2_gizclaw_api_key_state_config {
  /** Borrowed; deinit only after state destroy. */
  h2_gizclaw_service_t *service;
  const h2_pal_mem_api_t *mem;
  const h2_pal_sync_api_t *sync;
  const h2_pal_time_api_t *time;
  h2_gizclaw_str_t display_name; /**< Copied; same limits as API key create. */
  bool manage_api_keys;
  /** Nonzero total refresh budget, including queue/connect. */
  uint32_t timeout_ms;
} h2_gizclaw_api_key_state_config_t;

/** Caller-owned short copy. Never log the secret; erase the copy after use. */
typedef struct h2_gizclaw_api_key_snapshot {
  uint64_t revision; /**< Increments on every observable state change. */
  bool valid;        /**< key is meaningful only when true. */
  bool stale;        /**< Initial, refreshing, failed or closed state. */
  bool busy;         /**< One refresh chain is active. */
  bool closed;       /**< No further refresh accepted. */
  /** Last refresh result, or CLOSED after close. */
  h2_pal_result_t last_error;
  h2_gizclaw_api_key_t key;
} h2_gizclaw_api_key_snapshot_t;

/** Allocate an initially invalid, stale, idle state; sends no RPC.
 * On failure out_state is NULL. All config dependencies are required.
 */
h2_pal_result_t
h2_gizclaw_api_key_state_create(const h2_gizclaw_api_key_state_config_t *config,
                                h2_gizclaw_api_key_state_t **out_state);

/** Submit without waiting for RPC. Busy calls return OK and coalesce without
 * changing the current chain or extending its deadline. Closed returns CLOSED.
 * Optionally revoke the valid key first; OK/NOT_FOUND proceeds to create.
 * Revoke failure retains a valid, stale key; create failure invalidates it.
 * Submission errors are returned and stored as last_error. A refresh that has
 * expired is canceled first, then this call may start a new generation.
 */
h2_pal_result_t
h2_gizclaw_api_key_state_request_refresh(h2_gizclaw_api_key_state_t *state,
                                         bool revoke_current);

/** Read under the state mutex, without RPC or waiting for completion.
 * May run on any thread while the state remains alive. This and request_refresh
 * check the total deadline: expiration cancels the request, clears busy and
 * records TIMEOUT while retaining any existing key as stale. Clock/PAL errors
 * are returned with an empty output. Late completions cannot change the state.
 */
h2_pal_result_t
h2_gizclaw_api_key_state_snapshot(h2_gizclaw_api_key_state_t *state,
                                  h2_gizclaw_api_key_snapshot_t *out_snapshot);

/** Idempotently close and cancel without waiting; rejects future refresh and
 * discards late results. Retains an existing key as stale until destroy.
 * Does not revoke remotely: a key generated after cancellation/close may remain
 * on the server. No persistence or URL/QR formatting is performed.
 */
h2_pal_result_t
h2_gizclaw_api_key_state_close(h2_gizclaw_api_key_state_t *state);

/** Erase secrets and free, setting *state to NULL; NULL *state is harmless.
 * Returns BUSY without freeing while any accepted completion is undrained.
 * Teardown: close -> Service stop -> service_poll drain -> destroy -> Service
 * deinit. Never joins or waits for RPC; callers must exclude concurrent
 * readers.
 */
h2_pal_result_t
h2_gizclaw_api_key_state_destroy(h2_gizclaw_api_key_state_t **state);
#ifdef __cplusplus
}
#endif
#endif
