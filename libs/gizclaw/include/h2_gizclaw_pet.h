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

h2_pal_result_t h2_gizclaw_req_create_pet_get(h2_gizclaw_service_t *service,
                                              uint64_t identity,
                                              h2_gizclaw_str_t pet_name,
                                              uint32_t timeout_ms,
                                              h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_pet_get(const h2_gizclaw_req_t *request,
                              h2_gizclaw_resp_storage_t *storage,
                              h2_gizclaw_pet_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_pet_get(h2_gizclaw_service_t *service,
                                       h2_gizclaw_str_t pet_name,
                                       uint32_t timeout_ms,
                                       h2_gizclaw_resp_storage_t *storage,
                                       h2_gizclaw_pet_t *out_result);

h2_pal_result_t h2_gizclaw_req_create_pet_delete(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_pet_delete(const h2_gizclaw_req_t *request,
                                 h2_gizclaw_resp_storage_t *storage,
                                 h2_gizclaw_pet_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_pet_delete(h2_gizclaw_service_t *service,
                                          h2_gizclaw_str_t pet_name,
                                          uint32_t timeout_ms,
                                          h2_gizclaw_resp_storage_t *storage,
                                          h2_gizclaw_pet_t *out_result);

h2_pal_result_t h2_gizclaw_req_create_pet_adopt(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_pet_adopt_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_pet_adopt(const h2_gizclaw_req_t *request,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_pet_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_pet_adopt(
    h2_gizclaw_service_t *service,
    const h2_gizclaw_pet_adopt_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_pet_t *out_result);

h2_pal_result_t h2_gizclaw_req_create_pet_drive(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_pet_drive_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_pet_drive(const h2_gizclaw_req_t *request,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_pet_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_pet_drive(
    h2_gizclaw_service_t *service,
    const h2_gizclaw_pet_drive_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_pet_t *out_result);

h2_pal_result_t h2_gizclaw_req_create_pet_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_pet_list(const h2_gizclaw_req_t *request,
                               h2_gizclaw_resp_storage_t *storage,
                               h2_gizclaw_pet_page_t *out_result);
h2_pal_result_t h2_gizclaw_rpc_pet_list(h2_gizclaw_service_t *service,
                                        h2_gizclaw_str_t cursor, size_t limit,
                                        uint32_t timeout_ms,
                                        h2_gizclaw_resp_storage_t *storage,
                                        h2_gizclaw_pet_page_t *out_result);

h2_pal_result_t h2_gizclaw_req_create_pet_action_get(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_pet_action_get(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_pet_actions_t *out_result);
h2_pal_result_t
h2_gizclaw_rpc_pet_action_get(h2_gizclaw_service_t *service,
                              h2_gizclaw_str_t pet_name, uint32_t timeout_ms,
                              h2_gizclaw_resp_storage_t *storage,
                              h2_gizclaw_pet_actions_t *out_result);

/** Create a Pixa data-down request. Supply its writer to h2_gizclaw_req_do. */
h2_pal_result_t h2_gizclaw_req_create_pet_pixa_download(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request);
h2_pal_result_t
h2_gizclaw_resp_parse_pet_pixa_download(const h2_gizclaw_req_t *request,
                                        h2_gizclaw_resp_storage_t *storage,
                                        h2_gizclaw_pet_pixa_info_t *out_result);
/** Synchronous Pixa download. `write` runs from h2_gizclaw_service_poll() on
 * the App task, so call this from another task while the App keeps polling;
 * it never polls on its own. */
h2_pal_result_t h2_gizclaw_rpc_pet_pixa_download(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t pet_name,
    h2_gizclaw_pet_pixa_write_fn write, void *write_user, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_pet_pixa_info_t *out_result);
#ifdef __cplusplus
}
#endif
#endif
