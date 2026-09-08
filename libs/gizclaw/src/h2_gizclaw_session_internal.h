#ifndef H2_GIZCLAW_SESSION_INTERNAL_H
#define H2_GIZCLAW_SESSION_INTERNAL_H

#include "h2_gizclaw_session.h"

/* Session and its Service outlive all admitted RPC calls. */
h2_pal_result_t
h2_gizclaw_service_attach_session_internal(h2_gizclaw_service_t *service,
                                           h2_gizclaw_session_t *session);
h2_pal_result_t
h2_gizclaw_service_detach_session_internal(h2_gizclaw_service_t *service);
h2_pal_result_t
h2_gizclaw_service_acquire_session_internal(h2_gizclaw_service_t *service,
                                            h2_gizclaw_session_t **out);
void h2_gizclaw_service_release_session_internal(h2_gizclaw_service_t *service);
h2_pal_result_t h2_gizclaw_session_workspace_begin_internal(
    h2_gizclaw_session_t *session, h2_gizclaw_str_t name, uint32_t timeout_ms);
h2_pal_result_t h2_gizclaw_session_workspace_finish_internal(
    h2_gizclaw_session_t *session, h2_pal_result_t result,
    const h2_gizclaw_workspace_activation_t *activation,
    const h2_gizclaw_workspace_parameters_patch_t *parameters);

h2_pal_result_t h2_gizclaw_conversation_retarget_internal(
    h2_gizclaw_conversation_t *conversation, const char *workspace);

#endif
