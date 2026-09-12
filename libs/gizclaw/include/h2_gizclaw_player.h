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
/** One item a product queues locally. The URL is required and takes the same
 * HTTPS Ogg/Opus form the RPC accepts; title and source_ref are optional and
 * absent when len is zero. Spans rather than buffers: a 32-item array of the
 * wire item would put 40 KB on the caller's stack. */
typedef struct h2_gizclaw_player_playlist_entry {
  h2_gizclaw_str_t url;
  h2_gizclaw_str_t title;
  h2_gizclaw_str_t source_ref;
  /* The track length the product already knows, 0 when unknown. Only used
   * to aim a timed start (play_index_at); never reported as the status
   * duration. */
  uint64_t duration_ms;
} h2_gizclaw_player_playlist_entry_t;
/** Replace the playlist with one HTTPS Ogg/Opus URL and begin asynchronously.
 * Copies the URL before returning. OK means accepted, not playback completed.
 * Uses the same player, cancellation and telemetry as remote audio RPCs. */
h2_pal_result_t h2_gizclaw_player_play(h2_gizclaw_service_t *service,
                                       h2_gizclaw_str_t url);
/** Start the already-queued item the user picked, the same way the remote
 * client.device.audioplayer.play RPC selects one. An index at or past the
 * playlist length is INVALID_ARG and leaves playback untouched. Same as
 * play_index_at(service, index, 0). */
h2_pal_result_t h2_gizclaw_player_play_index(h2_gizclaw_service_t *service,
                                             uint32_t index);
/** Start queued item `index` start_ms into the track; 0 is play_index.
 * Needs the item's duration_ms from playlist_set: without it the item plays
 * from 0 and reports 0. With it the player fetches the headers with a
 * Range request, then a Range from about the start, lands on the next valid
 * Ogg page and continues to the exact start; a server that ignores Range, or
 * any failure before the first sample, falls back to one plain download that
 * skips to the start without decoding. status.position_ms shows start_ms
 * while buffering and then the position derived from the stream's granule
 * positions; a start beyond the real end completes the item there. Repeat
 * and end-of-track advance start the next item at 0. INVALID_ARG for a bad
 * index or start_ms at or past a known duration_ms, before anything is
 * touched; otherwise the same results as play_index. */
h2_pal_result_t h2_gizclaw_player_play_index_at(h2_gizclaw_service_t *service,
                                                uint32_t index,
                                                uint64_t start_ms);
/** Replace the playlist with a caller-owned array, the same way the remote
 * client.device.audioplayer.playlist.set RPC does: everything is validated
 * before the queue is touched, so a rejection preserves both the previous
 * playlist and playback, and the revision moves only on success. A count of
 * zero clears the playlist; a count above the ceiling is INVALID_ARG, since
 * no later moment makes it fit. Copies every span before returning.
 * Deliberately does not start playback: the write is pure on both paths, so a
 * product that wants the album to begin follows it with play_index(0). */
h2_pal_result_t h2_gizclaw_player_playlist_set(
    h2_gizclaw_service_t *service,
    const h2_gizclaw_player_playlist_entry_t *items, uint32_t count);
/** Select off, one or all, the values client.device.audioplayer.mode.set
 * accepts; anything else is INVALID_ARG and leaves the mode as it was. The
 * library owns end-of-track advance and looping, so a product picks the mode
 * here instead of re-implementing "next track, wrap at the end". */
h2_pal_result_t h2_gizclaw_player_repeat_set(h2_gizclaw_service_t *service,
                                             h2_gizclaw_str_t repeat);
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
