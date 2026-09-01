#ifndef H2_GIZCLAW_PET_H
#define H2_GIZCLAW_PET_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_gizclaw_pet_behavior {
  H2_GIZCLAW_PET_BEHAVIOR_NONE = 0,
  H2_GIZCLAW_PET_BEHAVIOR_FEED,
  H2_GIZCLAW_PET_BEHAVIOR_BATHE,
  H2_GIZCLAW_PET_BEHAVIOR_PLAY,
  H2_GIZCLAW_PET_BEHAVIOR_HEAL,
} h2_gizclaw_pet_behavior_t;

typedef enum h2_gizclaw_pet_lifecycle {
  H2_GIZCLAW_PET_LIFECYCLE_UNSPECIFIED = 0,
  H2_GIZCLAW_PET_LIFECYCLE_ALIVE,
  H2_GIZCLAW_PET_LIFECYCLE_DEAD,
} h2_gizclaw_pet_lifecycle_t;

typedef struct h2_gizclaw_pet_stats {
  double life;
  double health;
  double satiety;
  double hygiene;
  double mood;
  double energy;
} h2_gizclaw_pet_stats_t;

typedef struct h2_gizclaw_pet {
  char *name;
  char *pet_def_name;
  char *display_name;
  char *workspace_name;
  char *died_at;
  char *state_settled_at;
  char *updated_at;
  h2_gizclaw_pet_stats_t stats;
  int64_t experience;
  int64_t level;
  h2_gizclaw_pet_lifecycle_t lifecycle;
} h2_gizclaw_pet_t;

typedef struct h2_gizclaw_pet_page {
  h2_gizclaw_pet_t *items;
  size_t count;
  bool has_next;
  char *next_cursor;
} h2_gizclaw_pet_page_t;

typedef struct h2_gizclaw_pet_game_result {
  h2_gizclaw_str_t game_name;
  h2_gizclaw_str_t difficulty;
  h2_gizclaw_str_t outcome;
  h2_gizclaw_str_t occurred_at;
  int64_t score;
  int64_t max_score;
  int64_t duration_ms;
  bool has_score;
  bool has_max_score;
  bool has_duration_ms;
} h2_gizclaw_pet_game_result_t;

typedef struct h2_gizclaw_pet_drive_options {
  h2_gizclaw_str_t pet_name;
  h2_gizclaw_pet_behavior_t behavior;
  /** Optional game result. Mutually exclusive with behavior. */
  const h2_gizclaw_pet_game_result_t *game_result;
  /**
   * Stable for retries of one operation. Empty Drive and behavior use this
   * as the Drive key; game results use it as the game-result key.
   */
  h2_gizclaw_str_t idempotency_key;
} h2_gizclaw_pet_drive_options_t;

typedef struct h2_gizclaw_pet_adopt_options {
  /** Required caller-assigned Pet resource name. Reuse it when retrying. */
  h2_gizclaw_str_t name;
  /** Optional display name for a newly adopted Pet. */
  h2_gizclaw_str_t display_name;
} h2_gizclaw_pet_adopt_options_t;

typedef int (*h2_gizclaw_pet_pixa_write_fn)(void *user, const uint8_t *data,
                                            size_t len);

typedef struct h2_gizclaw_pet_pixa_info {
  char *pet_name;
  char *pet_def_name;
  char *source_path;
  uint64_t size_bytes;
  uint64_t received_bytes;
} h2_gizclaw_pet_pixa_info_t;

typedef struct h2_gizclaw_pet_clip_name {
  char *id;
  char *pixa_clip_name;
} h2_gizclaw_pet_clip_name_t;

typedef struct h2_gizclaw_pet_actions {
  char *pet_name;
  char *pet_def_name;
  char *feed;
  char *bathe;
  char *play;
  char *heal;
  char *idle;
  char *sick;
  char *dead;
  char *sleep;
  h2_gizclaw_pet_clip_name_t *clip_names;
  size_t clip_name_count;
} h2_gizclaw_pet_actions_t;

typedef struct h2_gizclaw_pet_request h2_gizclaw_pet_request_t;

typedef enum h2_gizclaw_pet_request_kind {
  H2_GIZCLAW_PET_LIST = 0,
  H2_GIZCLAW_PET_GET,
  H2_GIZCLAW_PET_ADOPT,
  H2_GIZCLAW_PET_DELETE,
  H2_GIZCLAW_PET_DRIVE,
  H2_GIZCLAW_PET_PIXA_DOWNLOAD,
  H2_GIZCLAW_PET_ACTIONS_GET,
} h2_gizclaw_pet_request_kind_t;

typedef struct h2_gizclaw_pet_result {
  h2_gizclaw_pet_request_kind_t kind;
  union {
    h2_gizclaw_pet_page_t page;
    h2_gizclaw_pet_t pet;
    h2_gizclaw_pet_pixa_info_t pixa;
    h2_gizclaw_pet_actions_t actions;
  } value;
} h2_gizclaw_pet_result_t;

typedef void (*h2_gizclaw_pet_completion_fn)(
    void *user, h2_gizclaw_pet_request_t *request);

h2_pal_result_t h2_gizclaw_service_pet_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_pet_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_pet_adopt_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_pet_adopt_options_t *options,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_pet_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_pet_drive_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_pet_drive_options_t *options,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_pet_pixa_download_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    h2_gizclaw_pet_pixa_write_fn write, void *write_user,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request);
h2_pal_result_t h2_gizclaw_service_pet_actions_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request);
h2_pal_result_t
h2_gizclaw_pet_request_cancel(h2_gizclaw_pet_request_t *request);
h2_pal_result_t h2_gizclaw_pet_request_wait(
    h2_gizclaw_pet_request_t *request, uint32_t timeout_ms);
const h2_gizclaw_operation_result_t *h2_gizclaw_pet_request_operation_result(
    const h2_gizclaw_pet_request_t *request);
const h2_gizclaw_pet_result_t *
h2_gizclaw_pet_request_response(const h2_gizclaw_pet_request_t *request);
void h2_gizclaw_pet_request_release(h2_gizclaw_pet_request_t *request);

#if defined(H2_GIZCLAW_TESTING)

int h2_gizclaw_client_pet_list(h2_gizclaw_client_t *client,
                               h2_gizclaw_str_t cursor, size_t limit,
                               h2_gizclaw_pet_page_t *out_page);
int h2_gizclaw_client_pet_get(h2_gizclaw_client_t *client,
                              h2_gizclaw_str_t pet_name,
                              h2_gizclaw_pet_t *out_pet);
int h2_gizclaw_client_pet_adopt(h2_gizclaw_client_t *client,
                                const h2_gizclaw_pet_adopt_options_t *options,
                                h2_gizclaw_pet_t *out_pet);
/**
 * @brief Delete one Pet and return the deleted Server snapshot.
 *
 * The call blocks until its request-scoped RPC completes. @p pet_name is a
 * borrowed, non-empty UTF-8 span. On success, @p out_pet owns its string fields
 * and must be released with h2_gizclaw_pet_deinit(). On every failure,
 * @p out_pet is empty.
 *
 * @param client Connected GizClaw client.
 * @param pet_name Pet resource name borrowed for the duration of the call.
 * @param out_pet Receives the owned deleted Pet snapshot.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG, H2_PAL_ERR_INVALID_STATE,
 * H2_PAL_ERR_NOT_FOUND, H2_PAL_ERR_UNSUPPORTED, H2_PAL_ERR_NO_MEMORY,
 * H2_PAL_ERR_FORMAT, or a transport error.
 */
int h2_gizclaw_client_pet_delete(h2_gizclaw_client_t *client,
                                 h2_gizclaw_str_t pet_name,
                                 h2_gizclaw_pet_t *out_pet);
int h2_gizclaw_client_pet_drive(h2_gizclaw_client_t *client,
                                const h2_gizclaw_pet_drive_options_t *options,
                                h2_gizclaw_pet_t *out_pet);
int h2_gizclaw_client_pet_pixa_download(h2_gizclaw_client_t *client,
                                        h2_gizclaw_str_t pet_name,
                                        h2_gizclaw_pet_pixa_write_fn write,
                                        void *write_user,
                                        h2_gizclaw_pet_pixa_info_t *out_info);
int h2_gizclaw_client_pet_actions_get(h2_gizclaw_client_t *client,
                                      h2_gizclaw_str_t pet_name,
                                      h2_gizclaw_pet_actions_t *out_actions);
#endif
const char *
h2_gizclaw_pet_actions_find_clip(const h2_gizclaw_pet_actions_t *actions,
                                 const char *id);

void h2_gizclaw_pet_deinit(h2_gizclaw_client_t *client, h2_gizclaw_pet_t *pet);
void h2_gizclaw_pet_page_deinit(h2_gizclaw_client_t *client,
                                h2_gizclaw_pet_page_t *page);
void h2_gizclaw_pet_pixa_info_deinit(h2_gizclaw_client_t *client,
                                     h2_gizclaw_pet_pixa_info_t *info);
void h2_gizclaw_pet_actions_deinit(h2_gizclaw_client_t *client,
                                   h2_gizclaw_pet_actions_t *actions);

#ifdef __cplusplus
}
#endif

#endif
