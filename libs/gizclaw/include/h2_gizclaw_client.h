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
  /** Request execution start through server EOS, normalized to at least one ms
   * when non-empty. Zero for an upload-only request. */
  uint64_t elapsed_ms;
  /** Download rate derived from `download_bytes` and `elapsed_ms`. */
  uint64_t download_bits_per_second;
  /** Exact upload body bytes accepted before the SDK received EOS. */
  uint64_t upload_bytes;
  /** Request execution start through the server EOS (remote-consumption
   * acknowledgement), normalized to at least one ms when non-empty.
   * Zero for a download-only request. */
  uint64_t upload_elapsed_ms;
  /** Upload rate derived from `upload_bytes` and `upload_elapsed_ms`. */
  uint64_t upload_bits_per_second;
} h2_gizclaw_speedtest_result_t;

typedef struct h2_gizclaw_service h2_gizclaw_service_t;
typedef struct h2_gizclaw_req h2_gizclaw_req_t;

/** Ping timing covers execution, not time spent waiting for app dispatch. */
h2_pal_result_t h2_gizclaw_req_create_ping(h2_gizclaw_service_t *service,
                                           uint64_t identity,
                                           uint32_t timeout_ms,
                                           h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_ping(const h2_gizclaw_req_t *request,
                           h2_gizclaw_ping_result_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_ping(h2_gizclaw_service_t *service,
                                    uint32_t timeout_ms,
                                    h2_gizclaw_ping_result_t *out_result);

/** Create one single-direction speed test; exactly one byte count must be
 * nonzero. Run upload and download as separate requests. Upload req_do needs
 * input_read; download req_do needs output_write. Downloads also validate the
 * server's repeating 0..255 benchmark
 * payload across frames, in addition to lengths and EOS. This is not a
 * cryptographic checksum; server EOS acknowledges upload consumption/length,
 * not upload contents. */
h2_pal_result_t h2_gizclaw_req_create_speedtest(
    h2_gizclaw_service_t *service, uint64_t identity, size_t upload_bytes,
    size_t download_bytes, uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
/** Copy byte counts and execution timings into caller-owned storage. */
h2_pal_result_t
h2_gizclaw_resp_parse_speedtest(const h2_gizclaw_req_t *request,
                                h2_gizclaw_speedtest_result_t *out_result);
/** Single-direction synchronous measurement from immediately before do to
 * successful wait return, including queue/control overhead but not parsing.
 * Unlike the req parser's execution duration, this also includes admission
 * queueing and the caller's wake-up latency. */
h2_pal_result_t
h2_gizclaw_rpc_speedtest(h2_gizclaw_service_t *service, size_t upload_bytes,
                         size_t download_bytes, uint32_t timeout_ms,
                         h2_gizclaw_speedtest_result_t *out_result);

h2_pal_result_t
h2_gizclaw_req_create_peer_delete(h2_gizclaw_service_t *service,
                                  uint64_t identity, uint32_t timeout_ms,
                                  h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_peer_delete(const h2_gizclaw_req_t *request);
h2_pal_result_t h2_gizclaw_rpc_peer_delete(h2_gizclaw_service_t *service,
                                           uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
