#ifndef H2_GIZCLAW_PROFILE_H
#define H2_GIZCLAW_PROFILE_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_service.h"
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

typedef struct h2_gizclaw_profile_request h2_gizclaw_profile_request_t;

/** Read the profile from a caller that exclusively owns and polls this client. */
int h2_gizclaw_client_profile_get(h2_gizclaw_client_t *client,
                                  h2_gizclaw_profile_t *out_profile);

/** Update only the profile name from an exclusive client-owner context. */
int h2_gizclaw_client_profile_put_name(h2_gizclaw_client_t *client,
                                       h2_gizclaw_str_t name,
                                       h2_gizclaw_profile_t *out_profile);

/** Update only the profile emoji from an exclusive client-owner context. */
int h2_gizclaw_client_profile_put_emoji(h2_gizclaw_client_t *client,
                                        h2_gizclaw_str_t emoji,
                                        h2_gizclaw_profile_t *out_profile);

/** Completion for one task-safe profile request. */
typedef void (*h2_gizclaw_profile_completion_fn)(
    void *user, h2_gizclaw_profile_request_t *request,
    const h2_gizclaw_operation_result_t *result,
    const h2_gizclaw_profile_t *profile);

/** Read the current caller's public profile without blocking the caller. */
h2_pal_result_t h2_gizclaw_service_profile_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    h2_gizclaw_profile_completion_fn completion, void *user,
    h2_gizclaw_profile_request_t **out_request);

/**
 * Update only DeviceProfile.name and return the server-confirmed profile.
 *
 * The request intentionally omits DeviceProfile.emoji, so a name change does
 * not overwrite the caller's existing avatar.
 */
h2_pal_result_t h2_gizclaw_service_profile_put_name_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_profile_completion_fn completion,
    void *user, h2_gizclaw_profile_request_t **out_request);

/**
 * Update only DeviceProfile.emoji and return the server-confirmed profile.
 *
 * The request intentionally omits DeviceProfile.name, so an avatar change does
 * not overwrite the caller's existing name.
 */
h2_pal_result_t h2_gizclaw_service_profile_put_emoji_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t emoji,
    uint32_t timeout_ms, h2_gizclaw_profile_completion_fn completion,
    void *user, h2_gizclaw_profile_request_t **out_request);

/** Request task-safe, idempotent cancellation of a profile request. */
h2_pal_result_t
h2_gizclaw_profile_request_cancel(h2_gizclaw_profile_request_t *request);

/** Release one terminal profile request. Calls before completion are ignored. */
void h2_gizclaw_profile_request_release(
    h2_gizclaw_profile_request_t *request);

#ifdef __cplusplus
}
#endif

#endif
