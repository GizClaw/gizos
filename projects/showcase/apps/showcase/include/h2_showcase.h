#ifndef H2_SHOWCASE_H
#define H2_SHOWCASE_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_SHOWCASE_COMPONENT_ACTION_BUTTON 1u

typedef struct h2_showcase_pointer_state {
  int32_t x;
  int32_t y;
  int pressed;
} h2_showcase_pointer_state_t;

typedef h2_pal_result_t (*h2_showcase_read_pointer_fn)(
    void *user, h2_showcase_pointer_state_t *out_state);

typedef struct h2_showcase_video_entry {
  const char *id;
  const char *display_name;
  const char *path;
  /** Signed 16-bit, 16 kHz, mono PCM from the same media timeline. */
  const void *audio_pcm_data;
  size_t audio_pcm_size;
} h2_showcase_video_entry_t;

typedef struct h2_showcase_character_entry {
  const char *id;
  const char *display_name;
} h2_showcase_character_entry_t;

typedef struct h2_showcase_config {
  const h2_showcase_video_entry_t *videos;
  size_t video_count;
  const h2_showcase_character_entry_t *characters;
  size_t character_count;
  const void *font_data;
  size_t font_size;
  /** GizClaw endpoint; NULL keeps the visual-only conversation simulation. */
  const char *gizclaw_server_endpoint;
  /** Base58 Peer private key dedicated to Showcase. */
  const char *gizclaw_private_key;
  /** Raw token that binds this Peer to the Showcase RuntimeProfile. */
  const char *gizclaw_registration_token;
  /** Required profile name returned by server.register, normally "showcase". */
  const char *gizclaw_runtime_profile_name;
  uint32_t gizclaw_connect_timeout_ms;
  /** Optional platform pointer reader; NULL consumes Runtime Touch PAL. */
  h2_showcase_read_pointer_fn read_pointer;
  void *pointer_user;
  int (*should_stop)(void *user);
  void *stop_user;
} h2_showcase_config_t;

h2_pal_result_t h2_showcase_run(h2_runtime_t *runtime,
                                const h2_showcase_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
