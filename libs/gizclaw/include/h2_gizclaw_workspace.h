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

typedef struct h2_gizclaw_workspace_history_audio_info {
  char *history_id;
  char *mime_type;
  char *workspace_name;
  uint64_t size_bytes;
  uint64_t received_bytes;
} h2_gizclaw_workspace_history_audio_info_t;

typedef int (*h2_gizclaw_workspace_history_audio_write_fn)(void *user,
                                                           const uint8_t *data,
                                                           size_t len);

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

typedef struct h2_gizclaw_workspace_request h2_gizclaw_workspace_request_t;

typedef enum h2_gizclaw_workspace_request_kind {
  H2_GIZCLAW_WORKSPACE_LIST = 0,
  H2_GIZCLAW_WORKSPACE_GET,
  H2_GIZCLAW_WORKSPACE_CREATE,
  H2_GIZCLAW_WORKSPACE_SET_INPUT,
  H2_GIZCLAW_WORKSPACE_DELETE,
  H2_GIZCLAW_WORKSPACE_ACTIVATE,
  H2_GIZCLAW_WORKSPACE_HISTORY_LIST,
  H2_GIZCLAW_WORKSPACE_HISTORY_AUDIO_DOWNLOAD,
} h2_gizclaw_workspace_request_kind_t;

typedef struct h2_gizclaw_workspace_get_result {
  h2_gizclaw_workspace_t workspace;
  char *runtime_profile_name;
  char *runtime_profile_revision;
} h2_gizclaw_workspace_get_result_t;

typedef struct h2_gizclaw_workspace_result {
  h2_gizclaw_workspace_request_kind_t kind;
  union {
    h2_gizclaw_workspace_page_t page;
    h2_gizclaw_workspace_get_result_t get;
    h2_gizclaw_workspace_t workspace;
    h2_gizclaw_workspace_activation_t activation;
    h2_gizclaw_workspace_history_page_t history_page;
    h2_gizclaw_workspace_history_audio_info_t history_audio;
  } value;
} h2_gizclaw_workspace_result_t;

typedef void (*h2_gizclaw_workspace_completion_fn)(
    void *user, h2_gizclaw_workspace_request_t *request);

h2_pal_result_t h2_gizclaw_service_workspaces_list_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t collection, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_workspace_completion_fn completion, void *user,
    h2_gizclaw_workspace_request_t **out_request);

h2_pal_result_t h2_gizclaw_service_workspace_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_completion_fn completion, void *user,
    h2_gizclaw_workspace_request_t **out_request);

h2_pal_result_t h2_gizclaw_service_workspace_create_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t collection, h2_gizclaw_str_t workflow_name,
    h2_gizclaw_str_t name, h2_gizclaw_workspace_completion_fn completion,
    void *user, h2_gizclaw_workspace_request_t **out_request);

h2_pal_result_t h2_gizclaw_service_workspace_set_input_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_input_mode_t input_mode,
    h2_gizclaw_workspace_completion_fn completion, void *user,
    h2_gizclaw_workspace_request_t **out_request);

h2_pal_result_t h2_gizclaw_service_workspace_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_completion_fn completion, void *user,
    h2_gizclaw_workspace_request_t **out_request);

h2_pal_result_t h2_gizclaw_service_workspace_activate_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_completion_fn completion, void *user,
    h2_gizclaw_workspace_request_t **out_request);

h2_pal_result_t h2_gizclaw_service_workspace_history_list_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace_name, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_workspace_history_order_t order,
    h2_gizclaw_workspace_completion_fn completion, void *user,
    h2_gizclaw_workspace_request_t **out_request);

h2_pal_result_t h2_gizclaw_service_workspace_history_audio_download_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace_name, h2_gizclaw_str_t history_id,
    h2_gizclaw_workspace_history_audio_write_fn write, void *write_user,
    h2_gizclaw_workspace_completion_fn completion, void *user,
    h2_gizclaw_workspace_request_t **out_request);

h2_pal_result_t
h2_gizclaw_workspace_request_cancel(h2_gizclaw_workspace_request_t *request);
h2_pal_result_t h2_gizclaw_workspace_request_wait(
    h2_gizclaw_workspace_request_t *request, uint32_t timeout_ms);
const h2_gizclaw_operation_result_t *
h2_gizclaw_workspace_request_operation_result(
    const h2_gizclaw_workspace_request_t *request);
const h2_gizclaw_workspace_result_t *h2_gizclaw_workspace_request_response(
    const h2_gizclaw_workspace_request_t *request);
void h2_gizclaw_workspace_request_release(
    h2_gizclaw_workspace_request_t *request);

#if defined(H2_GIZCLAW_TESTING)

int h2_gizclaw_client_workspaces_list(h2_gizclaw_client_t *client,
                                      h2_gizclaw_str_t collection,
                                      h2_gizclaw_str_t cursor, size_t limit,
                                      h2_gizclaw_workspace_page_t *out_page);

int h2_gizclaw_client_workspace_get(h2_gizclaw_client_t *client,
                                    h2_gizclaw_str_t name,
                                    h2_gizclaw_workspace_t *out_workspace,
                                    char **out_runtime_profile_name,
                                    char **out_runtime_profile_revision);

int h2_gizclaw_client_workspace_create(h2_gizclaw_client_t *client,
                                       h2_gizclaw_str_t collection,
                                       h2_gizclaw_str_t workflow_name,
                                       h2_gizclaw_str_t name,
                                       h2_gizclaw_workspace_t *out_workspace);

/**
 * @brief Set the client-selected input mode for one Workspace.
 *
 * The client reads the Workspace's existing typed parameters and updates only
 * their input mode. Workflow driver configuration remains server-owned and is
 * not exposed through this API. Callers must reload or activate the Workspace
 * before starting a new conversation with the updated mode.
 */
int h2_gizclaw_client_workspace_set_input(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_input_mode_t input_mode,
    h2_gizclaw_workspace_t *out_workspace);

/**
 * @brief Delete one Workspace and return the deleted Server snapshot.
 *
 * The call blocks until its request-scoped RPC completes. @p name is a
 * borrowed, non-empty UTF-8 span. On success, @p out_workspace owns its string
 * fields and must be released with h2_gizclaw_workspace_deinit(). On every
 * failure, @p out_workspace is empty.
 *
 * @param client Connected GizClaw client.
 * @param name Workspace name borrowed for the duration of the call.
 * @param out_workspace Receives the owned deleted Workspace snapshot.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG, H2_PAL_ERR_INVALID_STATE,
 * H2_PAL_ERR_NOT_FOUND, H2_PAL_ERR_UNSUPPORTED, H2_PAL_ERR_NO_MEMORY,
 * H2_PAL_ERR_FORMAT, or a transport error.
 */
int h2_gizclaw_client_workspace_delete(h2_gizclaw_client_t *client,
                                       h2_gizclaw_str_t name,
                                       h2_gizclaw_workspace_t *out_workspace);

int h2_gizclaw_client_workspace_activate(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_activation_t *out_activation);

int h2_gizclaw_client_workspace_history_list(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t workspace_name,
    h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_workspace_history_order_t order,
    h2_gizclaw_workspace_history_page_t *out_page);

int h2_gizclaw_client_workspace_history_audio_download(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t workspace_name,
    h2_gizclaw_str_t history_id,
    h2_gizclaw_workspace_history_audio_write_fn write, void *user,
    h2_gizclaw_workspace_history_audio_info_t *out_info);
#endif

bool h2_gizclaw_workspace_activation_ready(
    const h2_gizclaw_workspace_activation_t *activation,
    const char *expected_workspace_name);

void h2_gizclaw_workspace_deinit(h2_gizclaw_client_t *client,
                                 h2_gizclaw_workspace_t *workspace);
void h2_gizclaw_workspace_page_deinit(h2_gizclaw_client_t *client,
                                      h2_gizclaw_workspace_page_t *page);
void h2_gizclaw_workspace_activation_deinit(
    h2_gizclaw_client_t *client, h2_gizclaw_workspace_activation_t *activation);
void h2_gizclaw_workspace_history_page_deinit(
    h2_gizclaw_client_t *client, h2_gizclaw_workspace_history_page_t *page);
void h2_gizclaw_workspace_history_audio_info_deinit(
    h2_gizclaw_client_t *client,
    h2_gizclaw_workspace_history_audio_info_t *info);

#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_workspace_decode_activation_for_test(
    h2_gizclaw_client_t *client, const uint8_t *data, size_t len,
    h2_gizclaw_workspace_activation_t *out_activation);
int h2_gizclaw_workspace_decode_history_list_for_test(
    h2_gizclaw_client_t *client, const uint8_t *data, size_t len,
    size_t max_count, h2_gizclaw_workspace_history_page_t *out_page);
#endif

#ifdef __cplusplus
}
#endif

#endif
