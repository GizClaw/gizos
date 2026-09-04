#ifndef H2_GIZCLAW_PROFILE_H
#define H2_GIZCLAW_PROFILE_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_PROFILE_NAME_MAX_BYTES 256u
#define H2_GIZCLAW_PROFILE_EMOJI_MAX_BYTES 64u

/** Caller-owned snapshot returned by server.info.get/put. */
typedef struct h2_gizclaw_profile {
  bool has_name;
  char name[H2_GIZCLAW_PROFILE_NAME_MAX_BYTES + 1u];
  bool has_emoji;
  char emoji[H2_GIZCLAW_PROFILE_EMOJI_MAX_BYTES + 1u];
} h2_gizclaw_profile_t;

/** Create only: all strings are copied, and no network I/O is performed. */
h2_pal_result_t
h2_gizclaw_req_create_profile_get(h2_gizclaw_service_t *service,
                                  uint64_t identity, uint32_t timeout_ms,
                                  h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_req_create_profile_put_name(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_req_create_profile_put_emoji(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t emoji,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

/** Copy the matching terminal response into caller-owned storage.
 * Wrong request type, pending, remote failure and malformed payloads fail.
 * The returned snapshot remains valid after req_release(). */
h2_pal_result_t
h2_gizclaw_resp_parse_profile_get(const h2_gizclaw_req_t *request,
                                  h2_gizclaw_profile_t *out_profile);
h2_pal_result_t
h2_gizclaw_resp_parse_profile_put_name(const h2_gizclaw_req_t *request,
                                       h2_gizclaw_profile_t *out_profile);
h2_pal_result_t
h2_gizclaw_resp_parse_profile_put_emoji(const h2_gizclaw_req_t *request,
                                        h2_gizclaw_profile_t *out_profile);

/** Synchronous conveniences use the same request path and need no app poll.
 * timeout_ms bounds the wire RPC. Do not call on the service worker itself. */
h2_pal_result_t h2_gizclaw_rpc_profile_get(h2_gizclaw_service_t *service,
                                           uint32_t timeout_ms,
                                           h2_gizclaw_profile_t *out_profile);
h2_pal_result_t
h2_gizclaw_rpc_profile_put_name(h2_gizclaw_service_t *service,
                                h2_gizclaw_str_t name, uint32_t timeout_ms,
                                h2_gizclaw_profile_t *out_profile);
h2_pal_result_t
h2_gizclaw_rpc_profile_put_emoji(h2_gizclaw_service_t *service,
                                 h2_gizclaw_str_t emoji, uint32_t timeout_ms,
                                 h2_gizclaw_profile_t *out_profile);

#ifdef __cplusplus
}
#endif

#endif
