#ifndef H2_GIZCLAW_CLIENT_H
#define H2_GIZCLAW_CLIENT_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_rpc.h"
#include "h2_gizclaw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_gizclaw_ping_result {
  uint64_t round_trip_ms;
  int64_t server_time_ms;
} h2_gizclaw_ping_result_t;

/** A connection-scoped GizClaw notification. */
typedef enum h2_gizclaw_client_event_kind {
  H2_GIZCLAW_CLIENT_EVENT_NONE = 0,
  H2_GIZCLAW_CLIENT_EVENT_WORKSPACE_HISTORY_UPDATED,
} h2_gizclaw_client_event_kind_t;

/** Borrowed notification view, valid only for the callback invocation. */
typedef struct h2_gizclaw_client_event {
  h2_gizclaw_client_event_kind_t kind;
  h2_gizclaw_str_t workspace_name;
  int64_t last_updated_at_unix_ms;
} h2_gizclaw_client_event_t;

typedef void (*h2_gizclaw_client_event_fn)(
    void *user, const h2_gizclaw_client_event_t *event);

/** Maximum upload or download body accepted by one speed test. */
#define H2_GIZCLAW_SPEEDTEST_MAX_BYTES (1024u * 1024u * 1024u)

/** Result of one GizClaw speed test. */
typedef struct h2_gizclaw_speedtest_result {
  /** Exact download body bytes accepted before the SDK received EOS. */
  uint64_t download_bytes;
  /** Download duration, normalized to at least one ms when non-empty. */
  uint64_t elapsed_ms;
  /** Download rate derived from `download_bytes` and `elapsed_ms`. */
  uint64_t download_bits_per_second;
  /** Exact upload body bytes accepted before the SDK received EOS. */
  uint64_t upload_bytes;
  /** Upload duration, normalized to at least one ms when non-empty. */
  uint64_t upload_elapsed_ms;
  /** Upload rate derived from `upload_bytes` and `upload_elapsed_ms`. */
  uint64_t upload_bits_per_second;
} h2_gizclaw_speedtest_result_t;

typedef struct h2_gizclaw_service h2_gizclaw_service_t;
typedef struct h2_gizclaw_operation_result h2_gizclaw_operation_result_t;
typedef struct h2_gizclaw_ping_request h2_gizclaw_ping_request_t;
typedef struct h2_gizclaw_speedtest_request h2_gizclaw_speedtest_request_t;
typedef struct h2_gizclaw_peer_delete_request h2_gizclaw_peer_delete_request_t;

typedef void (*h2_gizclaw_ping_completion_fn)(
    void *user, h2_gizclaw_ping_request_t *request,
    const h2_gizclaw_operation_result_t *result,
    const h2_gizclaw_ping_result_t *ping);

typedef void (*h2_gizclaw_speedtest_completion_fn)(
    void *user, h2_gizclaw_speedtest_request_t *request,
    const h2_gizclaw_operation_result_t *result,
    const h2_gizclaw_speedtest_result_t *speedtest);

typedef void (*h2_gizclaw_peer_delete_completion_fn)(
    void *user, h2_gizclaw_peer_delete_request_t *request,
    const h2_gizclaw_operation_result_t *result);

/** Submit one Ping request to the service-owned network task. */
h2_pal_result_t h2_gizclaw_service_ping_async(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    h2_gizclaw_ping_completion_fn completion, void *user,
    h2_gizclaw_ping_request_t **out_request);

h2_pal_result_t
h2_gizclaw_ping_request_cancel(h2_gizclaw_ping_request_t *request);

void h2_gizclaw_ping_request_release(h2_gizclaw_ping_request_t *request);

int h2_gizclaw_client_init(const h2_gizclaw_config_t *config,
                           h2_gizclaw_client_t **out_client);

/**
 * Establish the GizClaw Peer connection and all mandatory transports.
 *
 * This blocking call registers bidirectional Opus media and returns success
 * only after the connection-scoped Direct Packet and Peer Event channels are
 * ready and the client owns the sole Peer Event access handle.
 */
int h2_gizclaw_client_connect(h2_gizclaw_client_t *client);

/**
 * Drive the connected GizClaw Peer for at most `timeout_ms` milliseconds.
 *
 * `H2_PAL_ERR_CLOSED` means a mandatory connection transport closed or the
 * Peer otherwise became unusable. Callers must close and deinit the complete
 * client before reconnecting; an individual transport is not reopened.
 */
int h2_gizclaw_client_poll(h2_gizclaw_client_t *client, int timeout_ms);

/**
 * Read and route at most one Peer Event from the connection-scoped stream.
 *
 * Conversation events are delivered to the active conversation's private
 * mailbox. Connection-scoped events are delivered to @p on_event. A timeout
 * or empty stream returns `H2_PAL_ERR_WOULD_BLOCK` or `H2_PAL_ERR_TIMEOUT`.
 * The client and all conversations must be used from one serialized worker.
 */
int h2_gizclaw_client_dispatch_event(h2_gizclaw_client_t *client,
                                     int timeout_ms,
                                     h2_gizclaw_client_event_fn on_event,
                                     void *event_user);

/** Set the connection-scoped event handler used when dispatch has no override.
 */
int h2_gizclaw_client_set_event_handler(h2_gizclaw_client_t *client,
                                        h2_gizclaw_client_event_fn on_event,
                                        void *event_user);
/** Submit a full-duplex speed test without polling on the calling task. */
h2_pal_result_t h2_gizclaw_service_speedtest_async(
    h2_gizclaw_service_t *service, uint64_t identity, size_t upload_bytes,
    size_t download_bytes, uint32_t timeout_ms,
    h2_gizclaw_speedtest_completion_fn completion, void *user,
    h2_gizclaw_speedtest_request_t **out_request);

h2_pal_result_t
h2_gizclaw_speedtest_request_cancel(h2_gizclaw_speedtest_request_t *request);

void h2_gizclaw_speedtest_request_release(
    h2_gizclaw_speedtest_request_t *request);

h2_pal_result_t h2_gizclaw_service_delete_peer_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_peer_delete_completion_fn completion, void *user,
    h2_gizclaw_peer_delete_request_t **out_request);
h2_pal_result_t h2_gizclaw_peer_delete_request_cancel(
    h2_gizclaw_peer_delete_request_t *request);
void h2_gizclaw_peer_delete_request_release(
    h2_gizclaw_peer_delete_request_t *request);

/**
 * Request idempotent deletion handoff for the authenticated Peer.
 *
 * This blocking call returns success only after the Server response and EOS
 * have been received. The Server may retire the connection after success.
 */
#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_client_delete_peer(h2_gizclaw_client_t *client);
#endif

/**
 * Close the complete Peer connection and invalidate active conversation leases.
 *
 * The caller must still deinit every conversation handle before deinitializing
 * the client.
 */
int h2_gizclaw_client_close(h2_gizclaw_client_t *client);

/**
 * Release the client.
 *
 * All conversation handles borrowing this client must already be deinitialized.
 */
void h2_gizclaw_client_deinit(h2_gizclaw_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
