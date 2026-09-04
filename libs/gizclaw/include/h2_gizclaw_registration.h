#ifndef H2_GIZCLAW_REGISTRATION_H
#define H2_GIZCLAW_REGISTRATION_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_REGISTRATION_NAME_CAPACITY 256u

/** Result returned after binding the connected Peer to a RuntimeProfile. */
typedef struct h2_gizclaw_registration_result {
  char runtime_profile_name[H2_GIZCLAW_REGISTRATION_NAME_CAPACITY];
} h2_gizclaw_registration_result_t;

/** Copy the token into a CREATED request; no registration is sent yet. */
h2_pal_result_t h2_gizclaw_req_create_register(h2_gizclaw_service_t *service,
                                               uint64_t identity,
                                               const char *token,
                                               uint32_t timeout_ms,
                                               h2_gizclaw_req_t **out_request);

/** Copy the server-confirmed Runtime Profile name into caller-owned storage. */
h2_pal_result_t
h2_gizclaw_resp_parse_register(const h2_gizclaw_req_t *request,
                               h2_gizclaw_registration_result_t *out_result);

/** Same request path, without a callback; no app poll loop is required. */
h2_pal_result_t
h2_gizclaw_rpc_register(h2_gizclaw_service_t *service, const char *token,
                        uint32_t timeout_ms,
                        h2_gizclaw_registration_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
