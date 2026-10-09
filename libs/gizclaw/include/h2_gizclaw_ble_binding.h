#ifndef H2_GIZCLAW_BLE_BINDING_H
#define H2_GIZCLAW_BLE_BINDING_H

#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2_gizclaw_api_key.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol version, identified by the UUID family as well as every frame. */
#define H2_GIZCLAW_BLE_BINDING_VERSION 1u
/** Maximum credential URL bytes, excluding its optional local NUL terminator.
 */
#define H2_GIZCLAW_BLE_BINDING_URL_MAX 1024u
/** INFO and REQUEST wire lengths in bytes. */
#define H2_GIZCLAW_BLE_BINDING_INFO_LEN 14u
#define H2_GIZCLAW_BLE_BINDING_REQUEST_LEN 13u
/** CREDENTIAL response header length; payload immediately follows it. */
#define H2_GIZCLAW_BLE_BINDING_HEADER_LEN 13u
/** Maximum response bytes; every response also fits negotiated ATT MTU - 2. */
#define H2_GIZCLAW_BLE_BINDING_FRAME_MAX 244u
/** Bounded undrained exposure records; a full queue prevents a new export. */
#define H2_GIZCLAW_BLE_BINDING_EXPOSURE_MAX 8u
/** INFO primary status flags; at most one is set (zero means idle not-ready).
 */
#define H2_GIZCLAW_BLE_BINDING_READY 0x01u
#define H2_GIZCLAW_BLE_BINDING_BUSY 0x02u
#define H2_GIZCLAW_BLE_BINDING_EXHAUSTED 0x04u
#define H2_GIZCLAW_BLE_BINDING_CLOSED 0x08u
#define H2_GIZCLAW_BLE_BINDING_FAILED 0x10u

/**
 * @brief Version 1 GATT wire contract, with UUID bytes in PAL/ATT little
 * endian.
 *
 * Service: 14edd4b0-91a6-4b0d-8c2f-b12be4f83b36 (primary).
 * INFO: 14edd4b1-91a6-4b0d-8c2f-b12be4f83b36 (Read).
 * REQUEST: 14edd4b2-91a6-4b0d-8c2f-b12be4f83b36 (Write, with response).
 * CREDENTIAL: 14edd4b3-91a6-4b0d-8c2f-b12be4f83b36 (Read).
 *
 * All integers are unsigned little endian; no frame has a trailing NUL.
 * INFO is {version:u8, flags:u8, revision:u64, url_len:u16,
 * max_payload:u16}. flags contains one primary status (or zero): CLOSED wins
 * over BUSY, then READY, EXHAUSTED (API-key create RESOURCE_EXHAUSTED), FAILED
 * (other terminal key/format error), or idle not-ready. Remaining bits are
 * zero. BUSY masks the previous generation's terminal error during a retry;
 * READY requires a valid, nonstale, idle key. No numeric server quota is
 * encoded. Not-ready INFO has url_len=0. revision is the live API-key snapshot
 * revision. max_payload=min(244, ATT_MTU-2)-13; unknown MTU uses 23 (8 payload
 * bytes). INFO readiness is transient and is not account binding or credential
 * receipt.
 *
 * REQUEST is {version:u8, expected_revision:u64, url_offset:u16,
 * payload_limit:u16}. Exact length, version=1, offset<=url_len, and a limit in
 * 1..231 are required. The service follows the first connection making a valid
 * REQUEST. Another connection's REQUEST/CREDENTIAL returns BUSY until that
 * connection disconnects or the window stops. One request can be pending:
 * repeating the identical request is an OK no-op; a different one returns BUSY.
 * INFO alone never claims a connection.
 *
 * CREDENTIAL is {version:u8, revision:u64, url_offset:u16, url_len:u16,
 * url_bytes...}. Payload length is inferred from frame length. One successful
 * read consumes its REQUEST, including an empty read at offset=url_len. The
 * payload is limited by REQUEST, MTU and callback output capacity. A callback
 * buffer smaller than the header returns NO_SPACE and leaves REQUEST pending.
 * A rejected/stale generation clears the pending request. Read/write access
 * offsets must be zero; the client uses REQUEST's offset, never Read Blob or
 * prepared/long writes. This remains valid on providers such as ESP NimBLE
 * which assemble a value using callback offset=0 before slicing it themselves.
 * Every response is shorter than ATT_MTU-1, terminating automatic long reads
 * without an EOF Read Blob probe. A client retries transport failures by
 * writing the REQUEST again.
 *
 * Clients read INFO, write REQUEST and read CREDENTIAL for each chunk, checking
 * version, exact revision/offset/total length and advancing by payload length.
 * Refresh, not-ready state, disconnect, or a rejected generation requires
 * discarding the partial URL and restarting at INFO. A complete URL is passed
 * to the same HTTPS API-key binding flow used by QR. This protocol does not
 * implement LiteLink's legacy x_proto topic 0x28 device token contract.
 */
extern const h2_pal_ble_uuid_t h2_gizclaw_ble_binding_service_uuid;
extern const h2_pal_ble_uuid_t h2_gizclaw_ble_binding_info_uuid;
extern const h2_pal_ble_uuid_t h2_gizclaw_ble_binding_request_uuid;
extern const h2_pal_ble_uuid_t h2_gizclaw_ble_binding_credential_uuid;

/** A caller-controlled, physically authorized credential disclosure window. */
typedef struct h2_gizclaw_ble_binding h2_gizclaw_ble_binding_t;

/** Dependencies remain borrowed through successful close. Text is copied. */
typedef struct h2_gizclaw_ble_binding_config {
  h2_gizclaw_api_key_state_t *api_key_state; /**< Same source as the QR path. */
  const h2_pal_ble_host_api_t *ble; /**< Caller starts/stops the shared Host. */
  const h2_pal_system_event_api_t *system_event;
  const h2_pal_mem_api_t *mem;
  const h2_pal_sync_api_t *sync;
  /** HTTPS origin (DNS/IPv4 host, optional port), at most 192 bytes.
   * Pass the HTTPS origin explicitly, never the GizClaw RPC endpoint/port.
   * No userinfo, path, query, fragment, whitespace or trailing slash.
   */
  h2_gizclaw_str_t server_origin;
  /** Optional asset identifier: [a-z0-9][a-z0-9_-]*, at most 32 bytes. */
  h2_gizclaw_str_t icon;
  /** Optional valid UTF-8 name, at most 40 Unicode scalar values / 160 bytes.
   * Control characters are rejected; query values are percent-encoded.
   */
  h2_gizclaw_str_t name;
  /** Optional complete BLE advertising name, at most 29 printable ASCII
   * bytes. Copied on open; separate from credential URL metadata. Contains
   * only public product/device display metadata, never a key or credential. */
  h2_gizclaw_str_t local_name;
} h2_gizclaw_ble_binding_config_t;

/** No secret. A record means bytes may have reached a phone, not confirmation.
 */
typedef struct h2_gizclaw_ble_binding_exposure {
  uint64_t revision; /**< Exact generation used for the response. */
  char key_name[27]; /**< API-key revoke identity; never the secret. */
} h2_gizclaw_ble_binding_exposure_t;

/** Caller-owned status, safe to retain; contains no secret or URL. */
typedef struct h2_gizclaw_ble_binding_snapshot {
  bool open;        /**< Window admits access. */
  bool advertising; /**< Own set's last accepted start / lifecycle event. */
  bool connected;   /**< A REQUEST has adopted a connection. */
  bool ready;       /**< Live key is valid, nonstale, idle and not closed. */
  uint64_t revision;
  size_t url_len;
  size_t pending_exposures;
  h2_pal_result_t last_error; /**< BLE operation/event or key/format failure. */
  uint8_t info_flags; /**< Same primary status and precedence as INFO. */
  bool has_rpc_error; /**< API-key state's original terminal RPC error. */
  int32_t
      rpc_error_code; /**< Canonical status, e.g. 8; never a server message. */
} h2_gizclaw_ble_binding_snapshot_t;

/** Format an HTTPS API-key URL with optional icon/name query parameters.
 * server_origin includes https://; default port spelling is preserved.
 * Required secret has gizclaw_sk_v1_ prefix, 15..95 URL-safe ASCII bytes.
 * Other text follows config's limits. Inputs and output must not overlap.
 * Pure bounded formatting, no allocation, RPC or PAL call. Output is NUL
 * terminated; out_len excludes NUL. Failure erases the output buffer and sets
 * out_len=0; insufficient capacity returns TRUNCATED. Never log the output.
 */
h2_pal_result_t
h2_gizclaw_ble_binding_format_url(h2_gizclaw_str_t server_origin,
                                  h2_gizclaw_str_t icon, h2_gizclaw_str_t name,
                                  h2_gizclaw_str_t secret, char *out,
                                  size_t capacity, size_t *out_len);

/** Allocate an idle window. Does not start Host, register GATT or send RPC.
 * Requires Mem, mutex-capable Sync, and System Event subscribe/unsubscribe.
 * On failure *out_binding is NULL; all text is validated and copied.
 */
h2_pal_result_t
h2_gizclaw_ble_binding_open(const h2_gizclaw_ble_binding_config_t *config,
                            h2_gizclaw_ble_binding_t **out_binding);

/** Open the authorized window: subscribe, register only this GATT service and
 * create/start a dedicated legacy connectable advertising set (100..200 ms),
 * containing its service UUID and optional local name. The legacy provider
 * places the name in its scan response. Repeated successful start is an OK no-op.
 * A partial failure seals admission and attempts stop; failed cleanup retains
 * the instance/resources for stop retry. A new start is BUSY until cleanup
 * succeeds. Providers must support handle sets and per-service unregistration;
 * no global/default-set/unregister-all fallback is performed.
 */
h2_pal_result_t h2_gizclaw_ble_binding_start(h2_gizclaw_ble_binding_t *binding);

/** Restart this set after a normal disconnect or set stop when no adopted peer
 * remains. Read live readiness using snapshot. No RPC, task, network wait or
 * key mutation. BLE PAL operations may have their provider's bounded latency.
 * Errors are retained in snapshot; keep polling/retry after transient errors.
 */
h2_pal_result_t h2_gizclaw_ble_binding_poll(h2_gizclaw_ble_binding_t *binding);

/** Thread-safe live read while instance is alive; failure empties output.
 * No RPC or BLE operation. closed/invalid/stale/busy keys are never ready.
 */
h2_pal_result_t h2_gizclaw_ble_binding_snapshot(
    h2_gizclaw_ble_binding_t *binding,
    h2_gizclaw_ble_binding_snapshot_t *out_snapshot);

/** Pop an exposure, including after stop. WOULD_BLOCK means empty.
 * A first nonempty credential response per key name/revision enqueues before
 * returning bytes to PAL; even partial transfer/provider send failure counts.
 * Deduplication persists across stop/start and draining for this instance.
 * A full queue returns NO_SPACE for a previously unrecorded generation before
 * copying credential bytes. Keep possibly exposed keys on page exit; revoke
 * unexposed/in-flight/orphan keys using the existing API-key state lifecycle.
 * Drain after stop to include a read racing with page exit. No secret returned.
 */
h2_pal_result_t h2_gizclaw_ble_binding_next_exposure(
    h2_gizclaw_ble_binding_t *binding,
    h2_gizclaw_ble_binding_exposure_t *out_exposure);

/** Seal admission, erase request state, stop/destroy only this advertising set,
 * unbind only this service, then unsubscribe. Never stop the shared Host,
 * disconnect shared physical links, or refresh/revoke/close the key state.
 * Idempotent. Cleanup errors retain every still-borrowed object for retry;
 * exposure records survive. Call outside GATT/system-event callbacks.
 */
h2_pal_result_t h2_gizclaw_ble_binding_stop(h2_gizclaw_ble_binding_t *binding);

/** Stop and free, setting *binding=NULL; NULL *binding is an OK no-op.
 * BUSY retains the sealed instance while exposure records are undrained;
 * drain them and retry. Other cleanup errors also retain it. Requires exclusive
 * caller access; registered callbacks are detached before freeing. Teardown
 * completes before key-state destroy or Host/dependency teardown. Lifecycle,
 * poll and next_exposure are serialized by one owner task; GATT/events and
 * snapshot may run concurrently, but not with successful close.
 */
h2_pal_result_t
h2_gizclaw_ble_binding_close(h2_gizclaw_ble_binding_t **binding);

#ifdef __cplusplus
}
#endif
#endif
