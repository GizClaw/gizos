#ifndef H2_GIZCLAW_WORKSPACE_H
#define H2_GIZCLAW_WORKSPACE_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES 255u
#define H2_GIZCLAW_WORKSPACE_PAGE_MAX_ITEMS 64u
#define H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES 255u
#define H2_GIZCLAW_WORKSPACE_HISTORY_TEXT_MAX_BYTES 4096u
#define H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS 64u

/** Automatically downloads Ogg/Opus history audio and delivers PCM16LE mono
 * at 16 kHz through the bound Track on the Service downlink task. Completion
 * means all PCM has been accepted by Track, not merely downloaded.
 * Cancellation quiesces Track delivery before the
 * request becomes terminal. No response parser or manual download API.
 * The compressed body is retained in PAL memory before playback; allocation
 * failure is reported by req_wait. PCM buffering is bounded to one packet. */
h2_pal_result_t h2_gizclaw_req_create_audio_play(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace_name, h2_gizclaw_str_t history_name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

typedef struct h2_gizclaw_workspace {
  char *name;
  /** Owned collection when the source operation is collection-scoped. */
  char *collection;
  char *workflow_name;
  bool system;
  bool available;
} h2_gizclaw_workspace_t;

typedef struct h2_gizclaw_workspace_page {
  h2_gizclaw_workspace_t *items;
  size_t count;
  bool has_next;
  char *next_cursor;
  char *runtime_profile_name;
  char *runtime_profile_revision;
} h2_gizclaw_workspace_page_t;

typedef enum h2_gizclaw_workspace_input_mode {
  H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK = 1,
  H2_GIZCLAW_WORKSPACE_INPUT_REALTIME = 2,
} h2_gizclaw_workspace_input_mode_t;

typedef enum h2_gizclaw_workspace_history_type {
  H2_GIZCLAW_WORKSPACE_HISTORY_UNSPECIFIED = 0,
  H2_GIZCLAW_WORKSPACE_HISTORY_GEAR = 1,
  H2_GIZCLAW_WORKSPACE_HISTORY_AGENT = 2,
} h2_gizclaw_workspace_history_type_t;

typedef enum h2_gizclaw_workspace_history_order {
  H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_ASC = 1,
  H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC = 2,
} h2_gizclaw_workspace_history_order_t;

typedef struct h2_gizclaw_workspace_history_entry {
  char *created_at;
  char *gear_id;
  /** History ID copied verbatim from the wire history entry name. */
  char *id;
  char *name;
  char *text;
  h2_gizclaw_workspace_history_type_t type;
  bool replay_available;
} h2_gizclaw_workspace_history_entry_t;

typedef struct h2_gizclaw_workspace_history_page {
  h2_gizclaw_workspace_history_entry_t *items;
  size_t count;
  bool available;
  bool has_next;
  char *message;
  char *next_cursor;
} h2_gizclaw_workspace_history_page_t;

typedef enum h2_gizclaw_workspace_runtime_state {
  H2_GIZCLAW_WORKSPACE_RUNTIME_UNSPECIFIED = 0,
  H2_GIZCLAW_WORKSPACE_RUNTIME_STOPPED = 1,
  H2_GIZCLAW_WORKSPACE_RUNTIME_STARTING = 2,
  H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING = 3,
  H2_GIZCLAW_WORKSPACE_RUNTIME_STOPPING = 4,
  H2_GIZCLAW_WORKSPACE_RUNTIME_ERROR = 5,
} h2_gizclaw_workspace_runtime_state_t;

typedef struct h2_gizclaw_workspace_activation {
  char *workspace_name;
  char *active_workspace_name;
  char *pending_workspace_name;
  char *workflow_name;
  h2_gizclaw_workspace_runtime_state_t runtime_state;
} h2_gizclaw_workspace_activation_t;

typedef struct h2_gizclaw_workspace_get_result {
  h2_gizclaw_workspace_t workspace;
  char *runtime_profile_name;
  char *runtime_profile_revision;
} h2_gizclaw_workspace_get_result_t;

/* create */
h2_pal_result_t h2_gizclaw_req_create_workspace_list(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t collection, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_workspace_get(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_workspace_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t collection, h2_gizclaw_str_t workflow_name,
    h2_gizclaw_str_t name, uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_workspace_set_input(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_input_mode_t input_mode, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_workspace_delete(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

/** Select the run workspace (SET only). Does not implicitly reload it. */
h2_pal_result_t h2_gizclaw_req_create_workspace_activate(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

/** Explicitly reload the currently selected run workspace. */
h2_pal_result_t
h2_gizclaw_req_create_workspace_reload(h2_gizclaw_service_t *service,
                                       uint64_t identity, uint32_t timeout_ms,
                                       h2_gizclaw_req_t **out_request);

h2_pal_result_t h2_gizclaw_req_create_workspace_history_list(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace_name, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_workspace_history_order_t order, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

/* parse */
h2_pal_result_t
h2_gizclaw_resp_parse_workspace_list(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_workspace_page_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_workspace_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_get_result_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_workspace_create(const h2_gizclaw_req_t *request,
                                       h2_gizclaw_resp_storage_t *storage,
                                       h2_gizclaw_workspace_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_workspace_set_input(const h2_gizclaw_req_t *request,
                                          h2_gizclaw_resp_storage_t *storage,
                                          h2_gizclaw_workspace_t *out_result);

h2_pal_result_t
h2_gizclaw_resp_parse_workspace_delete(const h2_gizclaw_req_t *request,
                                       h2_gizclaw_resp_storage_t *storage,
                                       h2_gizclaw_workspace_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_workspace_activate(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_activation_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_workspace_reload(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_activation_t *out_result);

h2_pal_result_t h2_gizclaw_resp_parse_workspace_history_list(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_history_page_t *out_result);

/* sync */
h2_pal_result_t h2_gizclaw_rpc_workspace_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t collection,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_page_t *out_result);

h2_pal_result_t
h2_gizclaw_rpc_workspace_get(h2_gizclaw_service_t *service,
                             h2_gizclaw_str_t name, uint32_t timeout_ms,
                             h2_gizclaw_resp_storage_t *storage,
                             h2_gizclaw_workspace_get_result_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_workspace_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t collection,
    h2_gizclaw_str_t workflow_name, h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_workspace_set_input(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_input_mode_t input_mode, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_workspace_delete(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_workspace_activate(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_activation_t *out_result);

h2_pal_result_t
h2_gizclaw_rpc_workspace_reload(h2_gizclaw_service_t *service,
                                uint32_t timeout_ms,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_workspace_activation_t *out_result);

h2_pal_result_t h2_gizclaw_rpc_workspace_history_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t workspace_name,
    h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_workspace_history_order_t order, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_history_page_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
