#ifndef H2_GIZCLAW_SESSION_INTERNAL_H
#define H2_GIZCLAW_SESSION_INTERNAL_H

#include "h2_gizclaw_session.h"

/* Call-owned snapshots: collect under owner locks, emit after the outermost
 * Session/audio lock is released. A control call records at most eight lines. */
typedef struct h2_gizclaw_audio_log {
  size_t count;
  h2_pal_log_level_t levels[8];
  char messages[8][H2_PAL_LOG_MESSAGE_MAX];
} h2_gizclaw_audio_log_t;

static inline char *h2_gizclaw_audio_log_append_internal(
    h2_gizclaw_audio_log_t *log, h2_pal_log_level_t level) {
  if (log->count == 8u)
    return NULL;
  log->levels[log->count] = level;
  return log->messages[log->count++];
}

void h2_gizclaw_service_flush_audio_log_internal(
    const h2_gizclaw_service_t *service, const h2_gizclaw_audio_log_t *log);
h2_pal_result_t h2_gizclaw_service_audio_control_internal(
    h2_gizclaw_service_t *service, bool start, h2_gizclaw_audio_log_t *log);
h2_pal_result_t h2_gizclaw_conversation_cancel_internal(
    h2_gizclaw_conversation_t *conversation, h2_gizclaw_audio_log_t *log);

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
