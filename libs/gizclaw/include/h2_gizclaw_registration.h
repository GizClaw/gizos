#ifndef H2_GIZCLAW_REGISTRATION_H
#define H2_GIZCLAW_REGISTRATION_H

#include "h2_gizclaw_types.h"
#include "h2_gizclaw_service.h"
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

typedef struct h2_gizclaw_registration_request
    h2_gizclaw_registration_request_t;

typedef void (*h2_gizclaw_registration_completion_fn)(
    void *user, h2_gizclaw_registration_request_t *request,
    const h2_gizclaw_operation_result_t *result,
    const h2_gizclaw_registration_result_t *registration);

/** Register from a caller that exclusively owns and polls this client. */
h2_pal_result_t
h2_gizclaw_client_register(h2_gizclaw_client_t *client, const char *token,
                           h2_gizclaw_registration_result_t *out_result);

/**
 * @brief Apply a stable pre-distributed RegistrationToken to a connected Peer.
 *
 * The token is encoded and copied before return. Firmware selection is a
 * separate channel-only request and is not part of registration.
 */
h2_pal_result_t h2_gizclaw_service_register_async(
    h2_gizclaw_service_t *service, uint64_t identity, const char *token,
    uint32_t timeout_ms, h2_gizclaw_registration_completion_fn completion,
    void *user, h2_gizclaw_registration_request_t **out_request);

h2_pal_result_t h2_gizclaw_registration_request_cancel(
    h2_gizclaw_registration_request_t *request);

void h2_gizclaw_registration_request_release(
    h2_gizclaw_registration_request_t *request);

#ifdef __cplusplus
}
#endif

#endif
