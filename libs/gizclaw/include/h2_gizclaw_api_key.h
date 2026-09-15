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
 * request_refresh/request_revoke/close and service_poll belong to the same
 * owner task. snapshot is thread-safe for any caller while the state is alive.
 * destroy requires exclusive access (no concurrent snapshot).
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
  bool busy;         /**< A refresh or revoke is active. */
  bool closed;       /**< No further refresh accepted. */
  /** Last refresh/revoke result, or CLOSED after close. */
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
 * extending its deadline; they clear a pending revoke-after request. Closed returns CLOSED.
 * Optionally revoke the valid key first; OK/NOT_FOUND proceeds to create.
 * Revoke failure retains a valid, stale key; create failure invalidates it.
 * Submission errors are returned and stored as last_error. A refresh that has
 * expired is detached as an orphan first, then this call may start a new generation.
 */
h2_pal_result_t
h2_gizclaw_api_key_state_request_refresh(h2_gizclaw_api_key_state_t *state,
                                         bool revoke_current);

/** Nonblocking revoke. Closed returns CLOSED; no valid key is an OK no-op.
 * When idle, revoke the valid key: OK/NOT_FOUND erases it; failure retains it
 * valid and stale with last_error. While refreshing, request that the new key
 * be revoked instead of exposed. A later busy request_refresh clears this flag.
 * The refresh then ends invalid, with OK unless create/revoke fails; a failed
 * revoke of an unexposed result never exposes its secret. Owner task only.
 */
h2_pal_result_t
h2_gizclaw_api_key_state_request_revoke(h2_gizclaw_api_key_state_t *state);

/** Read under the state mutex, without RPC or waiting for completion.
 * May run on any thread while the state remains alive. This and request_refresh
 * check the total deadline: expiration detaches the request, clears busy and
 * records TIMEOUT while retaining any existing key as stale. Clock/PAL errors
 * are returned with an empty output. Late creates never enter the snapshot:
 * successful orphan creates submit an internal best-effort revoke and their
 * secrets are erased immediately. A detached revoke that succeeds (or finds
 * NOT_FOUND) still clears the snapshot key it targeted.
 */
h2_pal_result_t
h2_gizclaw_api_key_state_snapshot(h2_gizclaw_api_key_state_t *state,
                                  h2_gizclaw_api_key_snapshot_t *out_snapshot);

/** Idempotently close admission without waiting or cancelling requests.
 * Retains an existing key as stale until destroy. In-flight requests become
 * orphans: successful creates submit a best-effort revoke even after close.
 * Submission may return CLOSED after Service stop; this is ignored. A create
 * whose response is lost to Service stop or transport failure cannot be revoked
 * by the device because it never learns the name. No persistence or URL/QR
 * formatting is performed.
 */
h2_pal_result_t
h2_gizclaw_api_key_state_close(h2_gizclaw_api_key_state_t *state);

/** Erase secrets and free, setting *state to NULL; NULL *state is harmless.
 * Returns BUSY without freeing while any accepted completion, including orphan revokes, is undrained.
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
