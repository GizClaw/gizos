#ifndef H2_GIZCLAW_PLAYER_H
#define H2_GIZCLAW_PLAYER_H
#include "h2_gizclaw_service.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Caller-owned snapshot, valid after the next command and Service teardown. */
typedef struct h2_gizclaw_player_status {
  char state[17];
  uint64_t position_ms;
  bool has_duration_ms;
  uint64_t duration_ms;
  char error_code[129];
  char error_message[513];
} h2_gizclaw_player_status_t;
/** Replace the playlist with one HTTPS Ogg/Opus URL and begin asynchronously.
 * Copies the URL before returning. OK means accepted, not playback completed.
 * Uses the same player, cancellation and telemetry as remote audio RPCs. */
h2_pal_result_t h2_gizclaw_player_play(h2_gizclaw_service_t *service,
                                       h2_gizclaw_str_t url);
h2_pal_result_t h2_gizclaw_player_stop(h2_gizclaw_service_t *service);
h2_pal_result_t h2_gizclaw_player_get_status(h2_gizclaw_service_t *service,
                                             h2_gizclaw_player_status_t *out);
#ifdef __cplusplus
}
#endif
#endif
