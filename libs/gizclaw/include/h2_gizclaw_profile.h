#ifndef H2_GIZCLAW_PROFILE_H
#define H2_GIZCLAW_PROFILE_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_PROFILE_NAME_MAX_BYTES 256u
#define H2_GIZCLAW_PROFILE_EMOJI_MAX_BYTES 64u

/** Caller-owned snapshot returned by server.info.get/put. */
typedef struct h2_gizclaw_profile {
  bool has_name;
  char name[H2_GIZCLAW_PROFILE_NAME_MAX_BYTES + 1u];
  bool has_emoji;
  char emoji[H2_GIZCLAW_PROFILE_EMOJI_MAX_BYTES + 1u];
} h2_gizclaw_profile_t;

/** Read the current caller's public profile on the GizClaw client owner task.
 */
int h2_gizclaw_client_profile_get(h2_gizclaw_client_t *client,
                                  h2_gizclaw_profile_t *out_profile);

/**
 * Update only DeviceProfile.name and return the server-confirmed profile.
 *
 * The request intentionally omits DeviceProfile.emoji, so a name change does
 * not overwrite the caller's existing avatar.
 */
int h2_gizclaw_client_profile_put_name(h2_gizclaw_client_t *client,
                                       h2_gizclaw_str_t name,
                                       h2_gizclaw_profile_t *out_profile);

/**
 * Update only DeviceProfile.emoji and return the server-confirmed profile.
 *
 * The request intentionally omits DeviceProfile.name, so an avatar change does
 * not overwrite the caller's existing name.
 */
int h2_gizclaw_client_profile_put_emoji(h2_gizclaw_client_t *client,
                                        h2_gizclaw_str_t emoji,
                                        h2_gizclaw_profile_t *out_profile);

#ifdef __cplusplus
}
#endif

#endif
