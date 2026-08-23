#ifndef H2_GIZCLAW_WORKSPACE_H
#define H2_GIZCLAW_WORKSPACE_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_types.h"
#include "h2_gizclaw_workflow.h"

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
 * The driver selects the upstream typed Workspace parameters object. The
 * The operation first reloads the client-owned Workspace and rejects any
 * driver-specific parameters beyond agent type and input. It then replaces
 * that minimal parameter object with the requested input mode, so existing
 * driver overrides can never be discarded silently. Callers must reload or
 * activate the Workspace before starting a new conversation with the updated
 * mode.
 */
int h2_gizclaw_client_workspace_set_input(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t name,
    h2_gizclaw_workflow_driver_t driver,
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

int h2_gizclaw_client_workspace_history_audio_get(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t workspace_name,
    h2_gizclaw_str_t history_id,
    h2_gizclaw_workspace_history_audio_write_fn write, void *user,
    h2_gizclaw_workspace_history_audio_info_t *out_info);

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
