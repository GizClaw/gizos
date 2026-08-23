#ifndef H2_GIZCLAW_REGISTRATION_H
#define H2_GIZCLAW_REGISTRATION_H

#include "h2_gizclaw_types.h"
#include "h2/pal/core/h2_pal_errors.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GIZCLAW_REGISTRATION_NAME_CAPACITY 256u

/** Result returned after binding the connected Peer to a RuntimeProfile. */
typedef struct h2_gizclaw_registration_result {
  char runtime_profile_name[H2_GIZCLAW_REGISTRATION_NAME_CAPACITY];
} h2_gizclaw_registration_result_t;

/**
 * @brief Apply a stable pre-distributed RegistrationToken to a connected Peer.
 *
 * This blocking call borrows @p token for its duration and clears
 * @p out_result before validation. The returned RuntimeProfile name is copied
 * into caller-owned storage. Firmware selection is a separate channel-only
 * request and is not part of registration.
 */
h2_pal_result_t
h2_gizclaw_client_register(h2_gizclaw_client_t *client, const char *token,
                           h2_gizclaw_registration_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
