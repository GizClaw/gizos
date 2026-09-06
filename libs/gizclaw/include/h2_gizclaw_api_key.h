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
#ifdef __cplusplus
}
#endif
#endif
