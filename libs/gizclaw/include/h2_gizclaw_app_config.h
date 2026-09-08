#ifndef H2_GIZCLAW_APP_CONFIG_H
#define H2_GIZCLAW_APP_CONFIG_H

#include "h2_gizclaw_service.h"
#include "h2_gizclaw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_APP_CONFIG_KEY_MAX_BYTES 63u
#define H2_GIZCLAW_APP_CONFIG_VALUE_MAX_BYTES 4096u
#define H2_GIZCLAW_APP_CONFIG_PAGE_MAX_ITEMS 64u
#define H2_GIZCLAW_APP_CONFIG_CURSOR_MAX_BYTES 171u

typedef struct h2_gizclaw_app_config_page {
  char **keys;
  size_t count;
  bool has_next;
  char *next_cursor;
  char *runtime_profile_name;
  char *runtime_profile_revision;
} h2_gizclaw_app_config_page_t;

typedef struct h2_gizclaw_app_config_value {
  /** Opaque UTF-8 bytes, with an extra NUL terminator. Use len, not strlen. */
  h2_gizclaw_owned_text_t value;
  char *runtime_profile_name;
  char *runtime_profile_revision;
} h2_gizclaw_app_config_value_t;

typedef struct h2_gizclaw_app_config_entry {
  char *key;
  h2_gizclaw_owned_text_t value;
} h2_gizclaw_app_config_entry_t;

/** Complete in-memory configuration at one server Profile revision. */
typedef struct h2_gizclaw_app_config_snapshot {
  h2_gizclaw_app_config_entry_t *items;
  size_t count;
  char *runtime_profile_name;
  char *runtime_profile_revision;
} h2_gizclaw_app_config_snapshot_t;

/** Read-only RuntimeProfile configuration. Create copies input without I/O.
 * limit is 1..64; an empty cursor starts the first page. */
h2_pal_result_t h2_gizclaw_req_create_app_config_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_req_create_app_config_get(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t key,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

/** Deep-copy a matching successful terminal response into caller storage.
 * Appends at storage.used; failure clears output and rolls back storage.
 * Results survive request release. An empty value is a successful value;
 * a missing key returns NOT_FOUND. No value interpretation is performed. */
h2_pal_result_t
h2_gizclaw_resp_parse_app_config_list(const h2_gizclaw_req_t *request,
                                      h2_gizclaw_resp_storage_t *storage,
                                      h2_gizclaw_app_config_page_t *out_result);
h2_pal_result_t
h2_gizclaw_resp_parse_app_config_get(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_app_config_value_t *out_result);

/** Blocking conveniences; never call on the Service worker/callback. */
h2_pal_result_t h2_gizclaw_rpc_app_config_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_app_config_page_t *out_result);
h2_pal_result_t
h2_gizclaw_rpc_app_config_get(h2_gizclaw_service_t *service,
                              h2_gizclaw_str_t key, uint32_t timeout_ms,
                              h2_gizclaw_resp_storage_t *storage,
                              h2_gizclaw_app_config_value_t *out_result);

#ifdef __cplusplus
}
#endif
#endif
