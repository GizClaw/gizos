#ifndef H2_GIZCLAW_WORKFLOW_H
#define H2_GIZCLAW_WORKFLOW_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_WORKFLOW_NAME_MAX_BYTES 63u
#define H2_GIZCLAW_WORKFLOW_COLLECTION_MAX_BYTES 63u
#define H2_GIZCLAW_WORKFLOW_LOCALE_MAX_BYTES 31u
#define H2_GIZCLAW_WORKFLOW_DISPLAY_NAME_MAX_BYTES 127u
#define H2_GIZCLAW_WORKFLOW_DESCRIPTION_MAX_BYTES 255u
#define H2_GIZCLAW_WORKFLOW_LANG_PAIR_MAX_BYTES 31u
#define H2_GIZCLAW_WORKFLOW_PAGE_MAX_ITEMS 64u
#define H2_GIZCLAW_WORKFLOW_I18N_MAX_ITEMS 8u

typedef struct h2_gizclaw_workflow_i18n {
  char *locale;
  char *display_name;
  char *description;
} h2_gizclaw_workflow_i18n_t;

/** Owned safe projection of one Runtime Profile workflow. */
typedef struct h2_gizclaw_workflow {
  char *collection;
  char *name;
  h2_gizclaw_workflow_i18n_t *i18n;
  size_t i18n_count;
  char *workspace_lang_pair;
} h2_gizclaw_workflow_t;

/** Page whose variable-sized fields live in caller-owned response storage. */
typedef struct h2_gizclaw_workflow_page {
  h2_gizclaw_workflow_t *items;
  size_t count;
  bool has_next;
  char *next_cursor;
  char *runtime_profile_name;
  char *runtime_profile_revision;
} h2_gizclaw_workflow_page_t;

typedef struct h2_gizclaw_workflow_get_result {
  h2_gizclaw_workflow_t workflow;
  char *runtime_profile_name;
  char *runtime_profile_revision;
} h2_gizclaw_workflow_get_result_t;

h2_pal_result_t h2_gizclaw_req_create_workflow_list(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t collection, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_req_create_workflow_get(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

/** Parsed strings, items and i18n entries live in caller-owned storage. */
h2_pal_result_t
h2_gizclaw_resp_parse_workflow_list(const h2_gizclaw_req_t *request,
                                    h2_gizclaw_resp_storage_t *storage,
                                    h2_gizclaw_workflow_page_t *out_page);
h2_pal_result_t h2_gizclaw_resp_parse_workflow_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workflow_get_result_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_workflow_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t collection,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workflow_page_t *out_page);
h2_pal_result_t
h2_gizclaw_rpc_workflow_get(h2_gizclaw_service_t *service,
                            h2_gizclaw_str_t name, uint32_t timeout_ms,
                            h2_gizclaw_resp_storage_t *storage,
                            h2_gizclaw_workflow_get_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
