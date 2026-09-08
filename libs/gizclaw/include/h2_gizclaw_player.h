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
  /* Appended so existing field-by-field initialisers keep compiling. Enough
   * playlist context to render "track 3 of 8" without a second call, and a
   * revision that tells a UI when its cached item list went stale. */
  bool has_current_index;
  uint32_t current_index;
  uint32_t playlist_length;
  uint32_t playlist_revision;
} h2_gizclaw_player_status_t;
/** The library's playlist ceiling, same bound the audio player RPCs enforce. */
#define H2_GIZCLAW_PLAYER_PLAYLIST_MAX_ITEMS 32u
/** One queued item as a UI needs it. The URL is deliberately absent: it is up
 * to 1 KB per item and only the library ever fetches it. */
typedef struct h2_gizclaw_player_playlist_item {
  bool has_title;
  char title[129];
  bool has_source_ref;
  char source_ref[129];
} h2_gizclaw_player_playlist_item_t;
/** Caller-owned view of the queue the device already holds. Copied whole so a
 * product can project a list without holding a library lock. */
typedef struct h2_gizclaw_player_playlist {
  h2_gizclaw_player_playlist_item_t items[H2_GIZCLAW_PLAYER_PLAYLIST_MAX_ITEMS];
  uint32_t item_count;
  /** Absent until a track is selected; a pushed playlist starts unselected. */
  bool has_current_index;
  uint32_t current_index;
  /** Moves on every set/append/local play; unchanged means nothing to redraw. */
  uint32_t playlist_revision;
  /** off, one or all. */
  char repeat[5];
} h2_gizclaw_player_playlist_t;
/** Replace the playlist with one HTTPS Ogg/Opus URL and begin asynchronously.
 * Copies the URL before returning. OK means accepted, not playback completed.
 * Uses the same player, cancellation and telemetry as remote audio RPCs. */
h2_pal_result_t h2_gizclaw_player_play(h2_gizclaw_service_t *service,
                                       h2_gizclaw_str_t url);
/** Start the already-queued item the user picked, the same way the remote
 * client.device.audioplayer.play RPC selects one. An index at or past the
 * playlist length is INVALID_ARG and leaves playback untouched. */
h2_pal_result_t h2_gizclaw_player_play_index(h2_gizclaw_service_t *service,
                                             uint32_t index);
h2_pal_result_t h2_gizclaw_player_stop(h2_gizclaw_service_t *service);
h2_pal_result_t h2_gizclaw_player_get_status(h2_gizclaw_service_t *service,
                                             h2_gizclaw_player_status_t *out);
/** Copy the current playlist. Never performs a network round trip; takes the
 * same device lock as the player commands. */
h2_pal_result_t h2_gizclaw_player_playlist_snapshot(
    h2_gizclaw_service_t *service, h2_gizclaw_player_playlist_t *out);
#ifdef __cplusplus
}
#endif
#endif
