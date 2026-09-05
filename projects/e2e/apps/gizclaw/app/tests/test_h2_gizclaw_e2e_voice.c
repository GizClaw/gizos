#include "h2_gizclaw_e2e_voice.h"
#include "h2_gizclaw_pcm_track_fake.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  NORMAL,
  SILENT_REPLY,
  MISSING_TEXT,
  REMOTE_ERROR,
  EARLY_VAD_END,
  MISSING_VAD_REPLY,
  SILENT_PLAY,
  EARLY_PLAY_END,
  IGNORE_PLAY_CANCEL,
  POLL_ERROR,
  UNSET_ERROR,
  NO_HISTORY,
  LOST_HISTORY,
  END_ERROR,
  BEGIN_ERROR,
  CREATE_ERROR,
  EMPTY_TEXT_DONE,
  EMPTY_REPLY,
  SET_ERROR,
  REPLACEMENT_SET_ERROR,
  REPLACEMENT_UNSET_ERROR,
  STALE_READ,
  STALE_WRITE,
  SILENT_REPLACEMENT,
  EMPTY_REPLACEMENT,
  REPLACEMENT_PLAY_ERROR,
  WRONG_GENERATION,
  DUPLICATE_COMPLETION,
  BAD_TERMINAL_RESULT,
  WRONG_TERMINAL_KIND,
  CANCEL_KEEPS_READING,
  CANCEL_KEEPS_WRITING,
  SECOND_END_ERROR,
  HISTORY_TOO_MANY,
  HISTORY_NULL_ITEMS,
  HISTORY_UNALIGNED_ITEMS,
  HISTORY_UNOWNED_ITEMS,
  HISTORY_NULL_ID,
  HISTORY_EMPTY_ID,
  HISTORY_UNTERMINATED_ID,
  HISTORY_LONG_ID,
  HISTORY_DUPLICATE_ID,
  HISTORY_UNOWNED_TEXT,
  HISTORY_UNTERMINATED_TEXT,
  HISTORY_UNAVAILABLE,
  HISTORY_PARTIAL_BASELINE,
  HISTORY_BAD_CURSOR,
  WORKSPACE_UNOWNED_NAME,
  ACTIVATION_UNTERMINATED_NAME,
  RESPONSE_USED_OVERFLOW,
  HISTORY_UNKNOWN_TYPE,
  GROUP_OLD_HISTORY,
  GROUP_WRONG_TYPE,
  GROUP_NO_AUDIO,
  GROUP_EARLY_FAILURE,
  GROUP_CANCEL_ERROR,
  HANGUP_ERROR,
  HANGUP_IGNORED,
  HANGUP_BAD_RESULT,
  HANGUP_BAD_KIND,
  HANGUP_CLOSES_PEER
};
static unsigned s_mode, s_live, s_allocs, s_fail_alloc;
static unsigned s_begins, s_replies, s_cancels, s_reconnects, s_play_creates;
static unsigned s_hangups, s_post_hangup_pings;
static uint64_t s_now, s_last_capture;
static bool s_realtime, s_reconnected;
static int s_service;
static uint8_t s_pcm[2560];
static h2_gizclaw_track_t *s_track;
static h2_gizclaw_track_t *s_detached_track;
static unsigned s_sets;
static bool s_emit;
static bool s_group;

static void assert_workspace(h2_gizclaw_str_t workspace) {
  const char *expected = s_group ? "group-workspace" : "test-workspace";
  assert(workspace.len == strlen(expected));
  assert(memcmp(workspace.data, expected, workspace.len) == 0);
}

struct h2_gizclaw_conversation {
  h2_gizclaw_conversation_callback_fn callback;
  h2_gizclaw_conversation_completion_fn completion;
  void *user;
  bool active, ended, cancelled;
  unsigned generation, replies;
  size_t input_bytes;
  uint64_t last_voice;
};
static h2_gizclaw_conversation_t *s_conversation;
struct h2_gizclaw_req {
  bool started, cancelled, complete;
  bool replacement;
  size_t written;
};

static void *allocate(void *user, size_t len) {
  (void)user;
  if (++s_allocs == s_fail_alloc)
    return NULL;
  void *p = malloc(len);
  if (p != NULL)
    ++s_live;
  return p;
}
static void release(void *user, void *p) {
  (void)user;
  if (p != NULL) {
    assert(s_live > 0u);
    --s_live;
    free(p);
  }
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate,
                                               .free = release};
static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
static int now(void *user, uint64_t *out) {
  (void)user;
  *out = s_now;
  return H2_PAL_OK;
}
static int sleep_ms(void *user, uint32_t ms) {
  (void)user;
  s_now += ms;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = now,
                                                 .sleep_ms = sleep_ms};
static const h2_pal_time_api_t time_api = {.vtable = &time_vtable};
h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *value) {
  return (h2_gizclaw_str_t){value, strlen(value)};
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  assert(symbol != NULL && stage != NULL);
  if (s_emit)
    printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n", symbol, stage,
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *fixture,
                                     uint32_t ms) {
  assert(fixture != NULL);
  return s_now + ms < 500000u;
}
int h2_gizclaw_e2e_fixture_reconnect_actor(h2_gizclaw_e2e_fixture_t *fixture,
                                           h2_gizclaw_e2e_actor_role_t role) {
  assert(role == H2_GIZCLAW_E2E_OWNER && fixture->case_state == NULL);
  assert(s_track == NULL && s_conversation == NULL);
  ++s_reconnects;
  s_reconnected = true;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_service_set_track(h2_gizclaw_service_t *service,
                                             h2_gizclaw_track_t *track) {
  assert(service == (h2_gizclaw_service_t *)&s_service && s_track == NULL);
  ++s_sets;
  if (s_mode == SET_ERROR || (s_sets == 3u && s_mode == REPLACEMENT_SET_ERROR))
    return H2_PAL_ERR_IO;
  s_track = track;
  return fake_pcm_track_bind(track);
}
h2_pal_result_t h2_gizclaw_service_unset_track(h2_gizclaw_service_t *service,
                                               h2_gizclaw_track_t *track) {
  assert(service == (h2_gizclaw_service_t *)&s_service && s_track == track);
  if (s_mode == UNSET_ERROR ||
      (s_sets == 3u && s_mode == REPLACEMENT_UNSET_ERROR))
    return H2_PAL_ERR_IO;
  if (s_sets == 2u)
    s_detached_track = track;
  fake_pcm_track_unbind(track);
  s_track = NULL;
  return H2_PAL_OK;
}
h2_pal_result_t
h2_gizclaw_conversation_create(h2_gizclaw_service_t *service,
                               h2_gizclaw_str_t workspace,
                               h2_gizclaw_conversation_callback_fn callback,
                               h2_gizclaw_conversation_completion_fn completion,
                               void *user, h2_gizclaw_conversation_t **out) {
  assert(service == (h2_gizclaw_service_t *)&s_service && workspace.len > 0u);
  assert_workspace(workspace);
  assert(s_conversation == NULL && callback != NULL && completion != NULL);
  *out = NULL;
  if (s_mode == CREATE_ERROR)
    return H2_PAL_ERR_NO_MEMORY;
  *out = allocate(NULL, sizeof(**out));
  if (*out == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  **out = (h2_gizclaw_conversation_t){
      .callback = callback, .completion = completion, .user = user};
  s_conversation = *out;
  return H2_PAL_OK;
}
h2_pal_result_t
h2_gizclaw_service_audio_start(h2_gizclaw_service_t *service) {
  assert(service == (h2_gizclaw_service_t *)&s_service);
  h2_gizclaw_conversation_t *value = s_conversation;
  assert(value == s_conversation && !value->active);
  if (s_mode == BEGIN_ERROR)
    return H2_PAL_ERR_IO;
  value->active = true;
  value->ended = value->cancelled = false;
  value->input_bytes = value->replies = 0u;
  ++value->generation;
  ++s_begins;
  s_last_capture = UINT64_MAX;
  return H2_PAL_OK;
}
h2_pal_result_t
h2_gizclaw_service_audio_end(h2_gizclaw_service_t *service) {
  assert(service == (h2_gizclaw_service_t *)&s_service);
  h2_gizclaw_conversation_t *value = s_conversation;
  assert(value->active);
  if (s_mode == END_ERROR || (s_mode == SECOND_END_ERROR && value->ended))
    return H2_PAL_ERR_IO;
  /* Realtime never submits a final utterance to obtain an invented EOS ack. */
  assert(!s_realtime);
  assert(value->input_bytes == sizeof(s_pcm));
  value->ended = true;
  return H2_PAL_OK;
}
h2_pal_result_t
h2_gizclaw_conversation_cancel(h2_gizclaw_conversation_t *value) {
  assert(value == s_conversation);
  ++s_cancels;
  if (s_realtime && value->replies == 2u) {
    ++s_hangups;
    if (s_mode == HANGUP_ERROR && s_hangups == 1u)
      return H2_PAL_ERR_IO;
  }
  if (s_mode == GROUP_CANCEL_ERROR)
    return H2_PAL_ERR_IO;
  value->cancelled = true;
  return H2_PAL_OK;
}
void h2_gizclaw_conversation_release(h2_gizclaw_conversation_t *value) {
  assert(value == s_conversation && !value->active);
  release(NULL, value);
  s_conversation = NULL;
}
static void complete(h2_gizclaw_conversation_t *value, bool cancelled) {
  value->active = false;
  h2_gizclaw_operation_result_t result = {
      .identity = value->generation,
      .terminal_kind = cancelled ? H2_GIZCLAW_OPERATION_CANCELED
                                 : H2_GIZCLAW_OPERATION_FINISHED,
      .result = cancelled ? H2_PAL_ERR_CLOSED : H2_PAL_OK};
  if (s_mode == WRONG_GENERATION)
    ++result.identity;
  if (s_mode == BAD_TERMINAL_RESULT || s_mode == GROUP_EARLY_FAILURE)
    result.result = H2_PAL_ERR_IO;
  if (s_mode == WRONG_TERMINAL_KIND)
    result.terminal_kind = H2_GIZCLAW_OPERATION_CANCELED;
  if (s_realtime && s_mode == HANGUP_BAD_RESULT)
    result.result = H2_PAL_ERR_IO;
  if (s_realtime && s_mode == HANGUP_BAD_KIND)
    result.terminal_kind = H2_GIZCLAW_OPERATION_FINISHED;
  value->completion(value->user, value, &result);
  if (s_mode == DUPLICATE_COMPLETION)
    value->completion(value->user, value, &result);
}
static int emit_reply(h2_gizclaw_conversation_t *value) {
  h2_gizclaw_conversation_event_t event = {.generation = value->generation};
  if (s_mode == REMOTE_ERROR) {
    event.kind = H2_GIZCLAW_CONVERSATION_EVENT_ERROR;
    return value->callback(value->user, value, &event);
  }
  if (s_mode != MISSING_TEXT) {
    if (s_mode == EMPTY_TEXT_DONE) {
      event.kind = H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA;
      event.text = "reply";
      event.text_len = 5u;
      int rc = value->callback(value->user, value, &event);
      if (rc != H2_PAL_OK)
        return rc;
    }
    event.kind = H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE;
    event.text =
        s_mode == EMPTY_TEXT_DONE || s_mode == EMPTY_REPLY ? "" : "reply";
    event.text_len = strlen(event.text);
    int rc = value->callback(value->user, value, &event);
    if (rc != H2_PAL_OK)
      return rc;
  }
  if (s_group) {
    ++value->replies;
    ++s_replies;
    return H2_PAL_OK;
  }
  uint8_t pcm[64] = {0};
  if (s_mode != SILENT_REPLY)
    pcm[0] = 1u;
  assert(fake_pcm_track_service_write(s_track, pcm, sizeof(pcm)) == H2_PAL_OK);
  event.kind = H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO_STARTED;
  int rc = value->callback(value->user, value, &event);
  if (rc != H2_PAL_OK)
    return rc;
  event.kind = H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE;
  rc = value->callback(value->user, value, &event);
  if (rc == H2_PAL_OK) {
    ++value->replies;
    ++s_replies;
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_service_poll(h2_gizclaw_service_t *service,
                                        size_t max, size_t *count) {
  assert(service == (h2_gizclaw_service_t *)&s_service && max > 0u);
  *count = 0u;
  if (s_mode == POLL_ERROR)
    return H2_PAL_ERR_IO;
  h2_gizclaw_conversation_t *value = s_conversation;
  if (value != NULL && !value->active && value->cancelled && s_track != NULL) {
    uint8_t pcm[64] = {1};
    if (s_mode == CANCEL_KEEPS_READING) {
      assert(fake_pcm_track_service_read(s_track, pcm, sizeof(pcm)) ==
             H2_PAL_OK);
    }
    if (s_mode == CANCEL_KEEPS_WRITING)
      assert(fake_pcm_track_service_write(s_track, pcm, sizeof(pcm)) ==
             H2_PAL_OK);
  }
  if (value == NULL || !value->active)
    return H2_PAL_OK;
  if (value->cancelled) {
    if (s_realtime && s_mode == HANGUP_IGNORED)
      return H2_PAL_OK;
    complete(value, true);
    *count = 1u;
    return H2_PAL_OK;
  }
  if (!value->ended) {
    uint8_t pcm[640];
    int rc = fake_pcm_track_service_read(s_track, pcm, sizeof(pcm));
    assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_WOULD_BLOCK);
    if (rc == H2_PAL_OK) {
      assert(s_last_capture == UINT64_MAX || s_now - s_last_capture >= 20u);
      s_last_capture = s_now;
      if (pcm[0] != 0u) {
        assert(memcmp(pcm, s_pcm + value->input_bytes % sizeof(s_pcm),
                      sizeof(pcm)) == 0);
        value->input_bytes += sizeof(pcm);
        value->last_voice = s_now;
      } else {
        assert(s_realtime);
        for (size_t i = 0u; i < sizeof(pcm); ++i)
          assert(pcm[i] == 0u);
      }
    }
  }
  if (!s_realtime && value->ended && (!s_group || value->replies == 0u)) {
    int rc = emit_reply(value);
    if (rc == H2_PAL_OK && (!s_group || s_mode == GROUP_EARLY_FAILURE))
      complete(value, false);
  } else if (s_realtime &&
             value->input_bytes == (value->replies + 1u) * sizeof(s_pcm) &&
             s_now - value->last_voice >= 500u && value->replies < 2u &&
             !(s_mode == MISSING_VAD_REPLY && value->replies == 1u)) {
    int rc = emit_reply(value);
    if (rc == H2_PAL_OK && s_mode == EARLY_VAD_END)
      complete(value, false);
  }
  return H2_PAL_OK;
}
static void response_reset(h2_gizclaw_resp_storage_t *storage) {
  assert(storage->used == 0u);
  memset(storage->data, 0xa5, storage->capacity);
}
static void *response_alloc(h2_gizclaw_resp_storage_t *storage, size_t size) {
  size_t alignment = _Alignof(max_align_t);
  size_t offset = (storage->used + alignment - 1u) / alignment * alignment;
  assert(offset <= storage->capacity && size <= storage->capacity - offset);
  void *result = storage->data + offset;
  memset(result, 0, size);
  storage->used = offset + size;
  return result;
}
static char *response_string(h2_gizclaw_resp_storage_t *storage,
                             const char *value) {
  size_t len = strlen(value) + 1u;
  char *result = response_alloc(storage, len);
  memcpy(result, value, len);
  return result;
}
h2_pal_result_t h2_gizclaw_rpc_workspace_set_input(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_input_mode_t mode, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out) {
  assert(service == (h2_gizclaw_service_t *)&s_service && timeout == 30000u &&
         storage != NULL);
  assert_workspace(name);
  assert(s_conversation == NULL || !s_conversation->active);
  s_realtime = mode == H2_GIZCLAW_WORKSPACE_INPUT_REALTIME;
  response_reset(storage);
  *out = (h2_gizclaw_workspace_t){.name = response_string(storage, name.data),
                                  .available = true};
  if (s_mode == WORKSPACE_UNOWNED_NAME)
    out->name = (char *)name.data;
  return H2_PAL_OK;
}
h2_pal_result_t
h2_gizclaw_rpc_workspace_activate(h2_gizclaw_service_t *service,
                                  h2_gizclaw_str_t name, uint32_t timeout,
                                  h2_gizclaw_resp_storage_t *storage,
                                  h2_gizclaw_workspace_activation_t *out) {
  (void)service;
  (void)timeout;
  assert_workspace(name);
  response_reset(storage);
  *out = (h2_gizclaw_workspace_activation_t){
      .workspace_name = response_string(storage, name.data),
      .active_workspace_name = response_string(storage, name.data),
      .runtime_state = H2_GIZCLAW_WORKSPACE_RUNTIME_STARTING};
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_workspace_reload(
    h2_gizclaw_service_t *service, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_activation_t *out) {
  (void)service;
  (void)timeout;
  const char *name = s_group ? "group-workspace" : "test-workspace";
  response_reset(storage);
  *out = (h2_gizclaw_workspace_activation_t){
      .workspace_name = response_string(storage, name),
      .active_workspace_name = response_string(storage, name),
      .runtime_state = H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING};
  if (s_mode == ACTIVATION_UNTERMINATED_NAME)
    memset(out->active_workspace_name, 'x', strlen(name) + 1u);
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_workspace_history_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t workspace,
    h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_workspace_history_order_t order, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_history_page_t *out) {
  assert(service == (h2_gizclaw_service_t *)&s_service && workspace.len > 0u &&
         cursor.len == 0u);
  assert_workspace(workspace);
  assert(limit == 64u && order == H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC &&
         timeout == 30000u && storage != NULL);
  response_reset(storage);
  *out = (h2_gizclaw_workspace_history_page_t){.available = true};
  if ((s_replies > 0u || s_mode == GROUP_OLD_HISTORY) && s_mode != NO_HISTORY &&
      !(s_mode == LOST_HISTORY && s_reconnected)) {
    size_t count =
        s_mode == HISTORY_DUPLICATE_ID || s_mode == HISTORY_UNKNOWN_TYPE ? 2u
                                                                         : 1u;
    h2_gizclaw_workspace_history_entry_t *items =
        response_alloc(storage, count * sizeof(*items));
    items[0] = (h2_gizclaw_workspace_history_entry_t){
        .type = s_group && s_mode != GROUP_WRONG_TYPE
                    ? H2_GIZCLAW_WORKSPACE_HISTORY_GEAR
                    : H2_GIZCLAW_WORKSPACE_HISTORY_AGENT,
        .replay_available = s_mode != GROUP_NO_AUDIO};
    /* Sequence allocations explicitly: the unterminated-ID fault must put
     * the ID at the arena end, independent of initializer evaluation order. */
    items[0].text = response_string(storage, "reply");
    if (s_group && s_mode == MISSING_TEXT)
      items[0].text = NULL;
    items[0].id = response_string(storage, "new-history");
    out->items = items;
    out->count = count;
    if (count == 2u) {
      items[1] = items[0];
      if (s_mode == HISTORY_UNKNOWN_TYPE) {
        items[0].id = response_string(storage, "future-history");
        items[0].type = (h2_gizclaw_workspace_history_type_t)127;
        items[0].text = NULL;
      }
    }
    switch (s_mode) {
    case HISTORY_TOO_MANY:
      out->count = 65u;
      break;
    case HISTORY_NULL_ITEMS:
      out->items = NULL;
      break;
    case HISTORY_UNALIGNED_ITEMS:
      out->items = (void *)((uint8_t *)items + 1u);
      break;
    case HISTORY_UNOWNED_ITEMS:
      out->items = (void *)&s_service;
      break;
    case HISTORY_NULL_ID:
      items[0].id = NULL;
      break;
    case HISTORY_EMPTY_ID:
      items[0].id[0] = '\0';
      break;
    case HISTORY_UNTERMINATED_ID:
      memset(items[0].id, 'x', 12u);
      break;
    case HISTORY_LONG_ID:
      items[0].id = response_alloc(storage, 257u);
      memset(items[0].id, 'x', 256u);
      break;
    case HISTORY_UNOWNED_TEXT:
      items[0].text = "unowned";
      break;
    case HISTORY_UNTERMINATED_TEXT:
      items[0].text = response_alloc(storage, 8u);
      memset(items[0].text, 'x', 8u);
      break;
    default:
      break;
    }
  }
  if (s_mode == HISTORY_UNAVAILABLE)
    out->available = false;
  if (s_mode == HISTORY_PARTIAL_BASELINE || s_mode == HISTORY_BAD_CURSOR) {
    out->has_next = true;
    out->next_cursor =
        s_mode == HISTORY_BAD_CURSOR ? NULL : response_string(storage, "next");
  }
  if (s_mode == RESPONSE_USED_OVERFLOW)
    storage->used = storage->capacity + 1u;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_rpc_ping(h2_gizclaw_service_t *service,
                                    uint32_t timeout,
                                    h2_gizclaw_ping_result_t *out) {
  assert(service == (h2_gizclaw_service_t *)&s_service && timeout > 0u);
  if (s_realtime && s_hangups != 0u) {
    assert(s_conversation != NULL && !s_conversation->active);
    ++s_post_hangup_pings;
    if (s_mode == HANGUP_CLOSES_PEER)
      return H2_PAL_ERR_CLOSED;
  }
  *out = (h2_gizclaw_ping_result_t){.round_trip_ms = 1u, .server_time_ms = 123};
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_create_audio_play(h2_gizclaw_service_t *service,
                                                 uint64_t identity,
                                                 h2_gizclaw_str_t workspace,
                                                 h2_gizclaw_str_t history,
                                                 uint32_t timeout,
                                                 h2_gizclaw_req_t **out) {
  assert(service == (h2_gizclaw_service_t *)&s_service &&
         (identity == 50u || identity == 51u || identity == 52u));
  assert(workspace.len > 0u && history.len == 11u && timeout == 30000u);
  *out = allocate(NULL, sizeof(**out));
  if (*out == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  **out = (h2_gizclaw_req_t){.replacement = identity == 52u};
  ++s_play_creates;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *request,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(!request->started && user == NULL && input_read == NULL &&
         output_write == NULL);
  request->started = true;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *request,
                                    uint32_t timeout) {
  assert(request->started);
  s_now += timeout < 20u ? timeout : 20u;
  if (request->cancelled && s_mode != IGNORE_PLAY_CANCEL)
    return H2_PAL_ERR_CLOSED;
  if (request->complete)
    return H2_PAL_OK;
  if (request->replacement && s_mode == REPLACEMENT_PLAY_ERROR)
    return H2_PAL_ERR_IO;
  if (request->replacement && s_mode == EMPTY_REPLACEMENT)
    return H2_PAL_OK;
  if (s_mode == EARLY_PLAY_END)
    return H2_PAL_OK;
  uint8_t pcm[64] = {0};
  if (s_mode != SILENT_PLAY &&
      !(request->replacement && s_mode == SILENT_REPLACEMENT))
    pcm[0] = 1u;
  if (request->replacement && s_mode == STALE_READ && request->written == 0u) {
    assert(fake_pcm_track_service_read(s_detached_track, pcm, sizeof(pcm)) ==
           H2_PAL_OK);
  }
  if (request->replacement && s_mode == STALE_WRITE && request->written == 0u)
    assert(fake_pcm_track_service_write(s_detached_track, pcm, sizeof(pcm)) ==
           H2_PAL_OK);
  int rc = fake_pcm_track_service_write(s_track, pcm, sizeof(pcm));
  if (rc == H2_PAL_OK) {
    request->written += sizeof(pcm);
    request->complete = request->written == 2048u;
    return request->complete ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
  }
  assert(rc == H2_PAL_ERR_WOULD_BLOCK);
  return H2_PAL_ERR_TIMEOUT;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *request) {
  request->cancelled = true;
  return H2_PAL_OK;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *request) {
  release(NULL, request);
}

static unsigned run_case(unsigned mode, int expected, unsigned fail_alloc) {
  assert(s_live == 0u && s_track == NULL && s_conversation == NULL);
  s_mode = mode;
  s_now = 0u;
  s_realtime = s_reconnected = false;
  s_sets = 0u;
  s_hangups = s_post_hangup_pings = 0u;
  s_detached_track = NULL;
  s_begins = s_replies = s_cancels = s_reconnects = s_play_creates = s_allocs =
      0u;
  s_fail_alloc = fail_alloc;
  h2_gizclaw_e2e_fixture_t *fixture = calloc(1u, sizeof(*fixture));
  assert(fixture != NULL);
  fixture->allocator = &mem;
  fixture->time = &time_api;
  fixture->pcm = s_pcm;
  fixture->pcm_len = sizeof(s_pcm);
  fixture->workspace_created = true;
  strcpy(fixture->workspace_name, "test-workspace");
  fixture->friend_group_created = true;
  strcpy(fixture->friend_group_workspace_name, "group-workspace");
  fixture->actors[0].service = (h2_gizclaw_service_t *)&s_service;
  if (s_emit)
    printf("H2_GIZCLAW_E2E stage=coverage-begin case=voice\n");
  char history_id[H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES + 1u] = "sentinel";
  int rc = s_group ? h2_gizclaw_e2e_generate_group_message(
                        fixture, history_id, sizeof(history_id))
                   : h2_gizclaw_e2e_run_voice(fixture);
  assert(rc == expected);
  assert(strcmp(fixture->workspace_name, "test-workspace") == 0);
  assert(fixture->workspace_created && fixture->friend_group_created);
  if (s_group)
    assert(strcmp(history_id, rc == H2_PAL_OK ? "new-history" : "") == 0);
  const unsigned allocations = s_allocs;
  if (mode == NORMAL && fail_alloc == 0u) {
    assert(s_begins == (s_group ? 1u : 3u) &&
           s_replies == (s_group ? 1u : 3u) && s_cancels == (s_group ? 1u : 2u));
    assert(s_hangups == (s_group ? 0u : 1u) &&
           s_post_hangup_pings == (s_group ? 0u : 1u));
    assert(s_play_creates == (s_group ? 0u : 3u) &&
           s_reconnects == (s_group ? 0u : 1u));
  }
  if (s_emit)
    printf("H2_GIZCLAW_E2E stage=coverage-end case=voice status=%s rc=%d "
           "cleanup_rc=%d\n",
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc,
           fixture->case_state == NULL ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE);
  if (mode == POLL_ERROR || mode == UNSET_ERROR ||
      mode == REPLACEMENT_UNSET_ERROR || mode == GROUP_CANCEL_ERROR ||
      mode == HANGUP_IGNORED) {
    assert(fixture->case_cleanup != NULL && fixture->case_state != NULL &&
           s_live > 0u);
    s_mode = NORMAL;
    assert(fixture->case_cleanup(fixture) == H2_PAL_OK);
  }
  assert(fixture->case_cleanup == NULL && fixture->case_state == NULL);
  assert(s_live == 0u && s_track == NULL && s_conversation == NULL);
  free(fixture);
  return allocations;
}
static int expected_result(unsigned mode) {
  switch (mode) {
  case NORMAL:
  case EMPTY_TEXT_DONE:
  case HISTORY_UNKNOWN_TYPE:
    return H2_PAL_OK;
  case REMOTE_ERROR:
    return H2_GIZCLAW_ERR_REMOTE;
  case MISSING_VAD_REPLY:
  case IGNORE_PLAY_CANCEL:
  case NO_HISTORY:
    return H2_PAL_ERR_TIMEOUT;
  case LOST_HISTORY:
    return H2_PAL_ERR_NOT_FOUND;
  case CREATE_ERROR:
    return H2_PAL_ERR_NO_MEMORY;
  case HISTORY_PARTIAL_BASELINE:
    return H2_PAL_ERR_NO_SPACE;
  case POLL_ERROR:
  case UNSET_ERROR:
  case END_ERROR:
  case BEGIN_ERROR:
  case SET_ERROR:
  case REPLACEMENT_SET_ERROR:
  case REPLACEMENT_UNSET_ERROR:
  case REPLACEMENT_PLAY_ERROR:
  case BAD_TERMINAL_RESULT:
  case SECOND_END_ERROR:
    return H2_PAL_ERR_IO;
  default:
    if (mode >= HISTORY_TOO_MANY && mode <= RESPONSE_USED_OVERFLOW &&
        mode != HISTORY_UNAVAILABLE)
      return H2_PAL_ERR_FORMAT;
    return H2_PAL_ERR_INVALID_STATE;
  }
}

int main(int argc, char **argv) {
  memset(s_pcm, 1, sizeof(s_pcm));
  if (argc == 3 && strcmp(argv[1], "--emit-voice-evidence") == 0) {
    s_emit = true;
    unsigned mode = (unsigned)atoi(argv[2]);
    assert(mode <= HISTORY_UNKNOWN_TYPE);
    run_case(mode, expected_result(mode), 0u);
    return 0;
  }
  unsigned allocations = run_case(NORMAL, H2_PAL_OK, 0u);
  run_case(SILENT_REPLY, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(MISSING_TEXT, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(REMOTE_ERROR, H2_GIZCLAW_ERR_REMOTE, 0u);
  run_case(EARLY_VAD_END, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(MISSING_VAD_REPLY, H2_PAL_ERR_TIMEOUT, 0u);
  run_case(SILENT_PLAY, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(EARLY_PLAY_END, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(IGNORE_PLAY_CANCEL, H2_PAL_ERR_TIMEOUT, 0u);
  run_case(POLL_ERROR, H2_PAL_ERR_IO, 0u);
  run_case(UNSET_ERROR, H2_PAL_ERR_IO, 0u);
  run_case(NO_HISTORY, H2_PAL_ERR_TIMEOUT, 0u);
  run_case(LOST_HISTORY, H2_PAL_ERR_NOT_FOUND, 0u);
  run_case(END_ERROR, H2_PAL_ERR_IO, 0u);
  run_case(BEGIN_ERROR, H2_PAL_ERR_IO, 0u);
  run_case(CREATE_ERROR, H2_PAL_ERR_NO_MEMORY, 0u);
  run_case(EMPTY_TEXT_DONE, H2_PAL_OK, 0u);
  run_case(EMPTY_REPLY, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(SET_ERROR, H2_PAL_ERR_IO, 0u);
  run_case(REPLACEMENT_SET_ERROR, H2_PAL_ERR_IO, 0u);
  run_case(REPLACEMENT_UNSET_ERROR, H2_PAL_ERR_IO, 0u);
  run_case(STALE_READ, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(STALE_WRITE, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(SILENT_REPLACEMENT, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(EMPTY_REPLACEMENT, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(REPLACEMENT_PLAY_ERROR, H2_PAL_ERR_IO, 0u);
  run_case(WRONG_GENERATION, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(DUPLICATE_COMPLETION, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(BAD_TERMINAL_RESULT, H2_PAL_ERR_IO, 0u);
  run_case(WRONG_TERMINAL_KIND, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(CANCEL_KEEPS_READING, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(CANCEL_KEEPS_WRITING, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(SECOND_END_ERROR, H2_PAL_ERR_IO, 0u);
  run_case(HANGUP_ERROR, H2_PAL_ERR_IO, 0u);
  run_case(HANGUP_IGNORED, H2_PAL_ERR_TIMEOUT, 0u);
  run_case(HANGUP_BAD_RESULT, H2_PAL_ERR_IO, 0u);
  run_case(HANGUP_BAD_KIND, H2_PAL_ERR_INVALID_STATE, 0u);
  run_case(HANGUP_CLOSES_PEER, H2_PAL_ERR_CLOSED, 0u);
  for (unsigned mode = HISTORY_TOO_MANY; mode <= HISTORY_UNKNOWN_TYPE; ++mode)
    run_case(mode, expected_result(mode), 0u);
  for (unsigned i = 1u; i <= allocations; ++i)
    run_case(NORMAL, H2_PAL_ERR_NO_MEMORY, i);
  s_group = true;
  allocations = run_case(NORMAL, H2_PAL_OK, 0u);
  const unsigned group_modes[] = {
      MISSING_TEXT, REMOTE_ERROR, POLL_ERROR, UNSET_ERROR, NO_HISTORY,
      END_ERROR, BEGIN_ERROR, CREATE_ERROR, SET_ERROR, SECOND_END_ERROR,
      HISTORY_TOO_MANY, HISTORY_NULL_ITEMS, HISTORY_UNALIGNED_ITEMS,
      HISTORY_UNOWNED_ITEMS, HISTORY_NULL_ID, HISTORY_EMPTY_ID,
      HISTORY_UNTERMINATED_ID, HISTORY_LONG_ID, HISTORY_DUPLICATE_ID,
      HISTORY_UNOWNED_TEXT, HISTORY_UNTERMINATED_TEXT, HISTORY_UNAVAILABLE,
      HISTORY_PARTIAL_BASELINE, HISTORY_BAD_CURSOR, WORKSPACE_UNOWNED_NAME,
      ACTIVATION_UNTERMINATED_NAME, RESPONSE_USED_OVERFLOW, HISTORY_UNKNOWN_TYPE,
      GROUP_OLD_HISTORY, GROUP_WRONG_TYPE, GROUP_NO_AUDIO,
      BAD_TERMINAL_RESULT, WRONG_GENERATION, DUPLICATE_COMPLETION,
      GROUP_EARLY_FAILURE, GROUP_CANCEL_ERROR};
  for (size_t i = 0u; i < sizeof(group_modes) / sizeof(group_modes[0]); ++i) {
    const unsigned mode = group_modes[i];
    const int expected = mode == MISSING_TEXT ? H2_PAL_OK
                         : (mode == GROUP_OLD_HISTORY || mode == GROUP_WRONG_TYPE ||
                            mode == GROUP_NO_AUDIO) ? H2_PAL_ERR_TIMEOUT
                         : mode >= GROUP_EARLY_FAILURE ? H2_PAL_ERR_IO
                         : expected_result(mode);
    run_case(mode, expected, 0u);
  }
  for (unsigned i = 1u; i <= allocations; ++i)
    run_case(NORMAL, H2_PAL_ERR_NO_MEMORY, i);
  h2_gizclaw_e2e_fixture_t fixture = {0};
  char history[H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES + 1u] = "sentinel";
  assert(h2_gizclaw_e2e_generate_group_message(NULL, history, sizeof(history)) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(history[0] == '\0');
  assert(h2_gizclaw_e2e_generate_group_message(&fixture, NULL, sizeof(history)) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_generate_group_message(&fixture, history, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  fixture.allocator = &mem;
  fixture.time = &time_api;
  fixture.pcm = s_pcm;
  fixture.pcm_len = sizeof(s_pcm);
  fixture.actors[0].service = (h2_gizclaw_service_t *)&s_service;
  for (unsigned mode = 0u; mode < 6u; ++mode) {
    fixture.friend_group_created = mode != 0u;
    memset(fixture.friend_group_workspace_name, 0,
           sizeof(fixture.friend_group_workspace_name));
    if (mode == 2u)
      memset(fixture.friend_group_workspace_name, 'x',
             sizeof(fixture.friend_group_workspace_name));
    if (mode >= 3u)
      strcpy(fixture.friend_group_workspace_name, "group-workspace");
    fixture.pcm_len = mode == 3u ? 1u : sizeof(s_pcm);
    fixture.case_state = mode == 4u ? &s_service : NULL;
    /* A retained state must not be replaced; all invalid input paths are
     * allocation-free and leave both cleanup targets untouched. */
    const unsigned before = s_allocs;
    if (mode == 5u)
      fixture.actors[0].service = NULL;
    assert(h2_gizclaw_e2e_generate_group_message(&fixture, history,
                                                  sizeof(history)) ==
           (mode == 4u ? H2_PAL_ERR_INVALID_STATE : H2_PAL_ERR_INVALID_ARG));
    assert(history[0] == '\0' && s_allocs == before);
    assert(fixture.case_state == (mode == 4u ? &s_service : NULL));
  }
  return 0;
}
