#ifndef H2_GIZCLAW_SESSION_INTERNAL_H
#define H2_GIZCLAW_SESSION_INTERNAL_H

#include "h2_gizclaw_session.h"

/* Call-owned snapshots: collect under owner locks, emit after the outermost
 * Session/audio lock is released. A control call records at most eight lines.
 */
typedef struct h2_gizclaw_audio_log {
  size_t count;
  h2_pal_log_level_t levels[8];
  char messages[8][H2_PAL_LOG_MESSAGE_MAX];
} h2_gizclaw_audio_log_t;

static inline char* h2_gizclaw_audio_log_append_internal(
    h2_gizclaw_audio_log_t* log, h2_pal_log_level_t level) {
  if (log->count == 8u) {
    /* Preserve an error return even when interruption filled the trace. */
    if (level == H2_PAL_LOG_ERROR)
      for (size_t i = 0u; i < log->count; ++i)
        if (log->levels[i] != H2_PAL_LOG_ERROR) {
          log->levels[i] = level;
          return log->messages[i];
        }
    return NULL;
  }
  log->levels[log->count] = level;
  return log->messages[log->count++];
}

void h2_gizclaw_service_flush_audio_log_internal(
    const h2_gizclaw_service_t* service, const h2_gizclaw_audio_log_t* log);
h2_pal_result_t h2_gizclaw_service_audio_control_internal(
    h2_gizclaw_service_t* service, bool start, h2_gizclaw_audio_log_t* log,
    bool* out_empty);
h2_pal_result_t h2_gizclaw_service_audio_adopt_internal(
    h2_gizclaw_service_t* service, h2_gizclaw_track_t* track,
    h2_gizclaw_pcm_capture_id_t id, h2_gizclaw_audio_log_t* log);
/* Session-only callbacks preserve copied terminal input facts after request
 * retirement. The notification callback only wakes the Session's Runtime; it
 * must not acquire the Session mutex or mutate Session state. It runs after the
 * input lock is released while the request's media lease still pins the route,
 * callback context and Session lifetime. */
typedef void (*h2_gizclaw_session_conversation_completion_fn)(
    void* user, h2_gizclaw_conversation_t* conversation,
    const h2_gizclaw_operation_result_t* result, bool ended, bool empty,
    size_t end_downlink_write_index);
typedef void (*h2_gizclaw_session_input_end_notify_fn)(void* user);
h2_pal_result_t h2_gizclaw_conversation_create_session_internal(
    h2_gizclaw_service_t* service, h2_gizclaw_str_t workspace,
    h2_gizclaw_conversation_callback_fn callback,
    h2_gizclaw_session_conversation_completion_fn completion,
    h2_gizclaw_session_input_end_notify_fn input_end_notify, void* user,
    h2_gizclaw_conversation_t** out_conversation);
/* Validate the expected route under audio_mutex through admission. An admitted
 * Speech request returns BUSY without consuming capture or changing its input.
 * NULL track/id select legacy input; nonzero id selects capture adoption. */
h2_pal_result_t h2_gizclaw_conversation_audio_control_internal(
    h2_gizclaw_conversation_t* conversation, bool start,
    h2_gizclaw_track_t* track, h2_gizclaw_pcm_capture_id_t id,
    h2_gizclaw_audio_log_t* log, bool* out_empty);
/* Observe acknowledged END on the expected conversation, retaining final
 * managed facts after request retirement until that route starts again. */
h2_pal_result_t h2_gizclaw_conversation_audio_end_snapshot_internal(
    h2_gizclaw_conversation_t* conversation, bool* ended, bool* empty,
    size_t* end_downlink_write_index);
/* Track byte-write position; use the same units as the capture's ACK-time
 * end_downlink_write_index. No Track access escapes the Service borrow. */
size_t h2_gizclaw_service_pcm_downlink_writes_internal(
    h2_gizclaw_service_t* service);
typedef enum h2_gizclaw_cancel_source {
  H2_GIZCLAW_CANCEL_UNSPECIFIED,
  H2_GIZCLAW_CANCEL_API,
  H2_GIZCLAW_CANCEL_RESTART,
  H2_GIZCLAW_CANCEL_WORKSPACE,
  H2_GIZCLAW_CANCEL_REALTIME_END,
} h2_gizclaw_cancel_source_t;
h2_pal_result_t h2_gizclaw_conversation_cancel_internal(
    h2_gizclaw_conversation_t* conversation, h2_gizclaw_audio_log_t* log,
    int source);
/* Admit one complete UTF-8 text input (1..TEXT_MAX_BYTES, no NUL) on an idle
 * route; the text is copied. The Service worker sends a control BOS and a
 * TEXT_DONE that ends the input; completion follows through service_poll.
 * BUSY while any input, Speech or playback owns the route. Only the Session
 * calls this, with its own lock held; it records, never flushes, logs. */
h2_pal_result_t h2_gizclaw_conversation_send_text_internal(
    h2_gizclaw_conversation_t* conversation, h2_gizclaw_str_t text,
    h2_gizclaw_audio_log_t* log);

/* Session and its Service outlive all admitted RPC calls. */
h2_pal_result_t h2_gizclaw_service_attach_session_internal(
    h2_gizclaw_service_t* service, h2_gizclaw_session_t* session);
h2_pal_result_t h2_gizclaw_service_detach_session_internal(
    h2_gizclaw_service_t* service);
h2_pal_result_t h2_gizclaw_service_acquire_session_internal(
    h2_gizclaw_service_t* service, h2_gizclaw_session_t** out);
void h2_gizclaw_service_release_session_internal(h2_gizclaw_service_t* service);
h2_pal_result_t h2_gizclaw_session_workspace_begin_internal(
    h2_gizclaw_session_t* session, h2_gizclaw_str_t name, uint32_t timeout_ms);
h2_pal_result_t h2_gizclaw_session_workspace_finish_internal(
    h2_gizclaw_session_t* session, h2_pal_result_t result,
    const h2_gizclaw_workspace_activation_t* activation,
    const h2_gizclaw_workspace_parameters_patch_t* parameters, bool reloaded);
/* Serialize storage-only updates without stopping the current conversation.
 * A possible fence write makes later omitted reload confirmation unknown. */
h2_pal_result_t h2_gizclaw_session_parameters_begin_internal(
    h2_gizclaw_session_t* session, h2_gizclaw_str_t name,
    const h2_gizclaw_workspace_parameters_patch_t* parameters);
h2_pal_result_t h2_gizclaw_session_parameters_finish_internal(
    h2_gizclaw_session_t* session, h2_pal_result_t result);
/* Workspace delete participates only when name is the Session's current
 * Workspace: that path stops the conversation and owns the serialized
 * workspace RPC slot. Other names leave the Session untouched. A closed
 * Session rejects every delete. out_participating selects the finish call. */
h2_pal_result_t h2_gizclaw_session_workspace_delete_begin_internal(
    h2_gizclaw_session_t* session, h2_gizclaw_str_t name, uint32_t timeout_ms,
    bool* out_participating);
/* Run stop always participates, even without a current Workspace, and uses
 * workspace_delete_finish_internal for the same EMPTY/FAILED transition. */
h2_pal_result_t h2_gizclaw_session_run_stop_begin_internal(
    h2_gizclaw_session_t* session, uint32_t timeout_ms);
/* Success publishes EMPTY with no current Workspace, Workflow or confirmed
 * parameters; any failure, including an uncertain result, publishes FAILED. */
h2_pal_result_t h2_gizclaw_session_workspace_delete_finish_internal(
    h2_gizclaw_session_t* session, h2_pal_result_t result);

/* Interrupt the downlink: close it until the next downstream audio BOS,
 * forget the active stream, and drop what is buffered: queued Opus, the
 * decoder state and the Track's unplayed PCM (unless another owner holds the
 * Track downlink). */
void h2_gizclaw_conversation_downlink_interrupt_internal(
    h2_gizclaw_service_t* service);
/* Decoded chunks written to the Track so far; changes when sound arrives. */
size_t h2_gizclaw_conversation_downlink_writes_internal(
    h2_gizclaw_service_t* service);

h2_pal_result_t h2_gizclaw_conversation_retarget_internal(
    h2_gizclaw_conversation_t* conversation, const char* workspace);

#endif
