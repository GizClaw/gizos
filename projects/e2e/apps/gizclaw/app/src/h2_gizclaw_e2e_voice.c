#include "h2_gizclaw_e2e_voice.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define VOICE_TIMEOUT_MS 90000u
#define HISTORY_TIMEOUT_MS 30000u
#define DISPOSE_TIMEOUT_MS 5000u
#define GROUP_TALK_GRACE_MS 1500u
#define RESPONSE_BYTES 65536u
#define FRAME_BYTES 640u

typedef struct history_snapshot {
  char ids[H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS]
          [H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES + 1u];
  size_t count;
} history_snapshot_t;

typedef struct voice_state {
  h2_gizclaw_e2e_fixture_t *fixture;
  h2_gizclaw_service_t *service;
  const char *workspace_name;
  bool group_talk;
  h2_gizclaw_conversation_t *conversation;
  h2_gizclaw_req_t *play;
  h2_gizclaw_track_t *track;
  h2_gizclaw_track_t *replacement_track;
  uint8_t mic_pending[FRAME_BYTES];
  size_t mic_pending_len, mic_voice_len;
  uint64_t speaker_next_ms;
  bool mic_write_reported, speaker_read_reported;
  bool replacement_bound;
  bool bound, realtime, capture_clock_started;
  uint64_t capture_started_ms, emitted_bytes;
  size_t read_offset;
  unsigned read_round;
  atomic_uint clips_allowed;
  atomic_bool capture_enabled, block_playback, active;
  atomic_size_t captured, written, write_attempts;
  atomic_size_t read_attempts, replacement_written;
  atomic_bool replacement_non_silent;
  atomic_bool non_silent;
  atomic_int hook_error, terminal_result, terminal_kind;
  atomic_uint completions, rounds;
  uint64_t generation;
  bool round_text_seen, round_text_done, round_non_silent;
  size_t round_audio;
  size_t hook_audio_total;
  uint8_t *response;
  h2_gizclaw_resp_storage_t storage;
  history_snapshot_t before;
} voice_state_t;

static _Thread_local voice_state_t *s_polling;

static int evidence(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

static bool response_owns(const h2_gizclaw_resp_storage_t *storage,
                          const void *pointer, size_t size) {
  const uintptr_t base = (uintptr_t)storage->data;
  const uintptr_t value = (uintptr_t)pointer;
  return storage->data != NULL && pointer != NULL &&
         storage->used <= storage->capacity && value >= base &&
         value - base <= storage->used &&
         size <= storage->used - (value - base);
}

static bool response_text(const h2_gizclaw_resp_storage_t *storage,
                          const char *value, bool required, size_t maximum) {
  if (value == NULL)
    return !required;
  if (!response_owns(storage, value, 1u))
    return false;
  const size_t left =
      storage->used - ((uintptr_t)value - (uintptr_t)storage->data);
  const char *end = memchr(value, '\0', left);
  return end != NULL && (size_t)(end - value) <= maximum &&
         (!required || end != value);
}

static int clock_now(voice_state_t *state, uint64_t *out) {
  return h2_pal_time_get_monotonic_ms(state->fixture->time, out);
}

static int within(voice_state_t *state, uint64_t started, uint32_t limit) {
  uint64_t now = 0u;
  int rc = clock_now(state, &now);
  if (rc != H2_PAL_OK)
    return rc;
  return now >= started && now - started < limit &&
                 h2_gizclaw_e2e_fixture_has_time(state->fixture, 1u)
             ? H2_PAL_OK
             : H2_PAL_ERR_TIMEOUT;
}

static int poll_service(voice_state_t *state) {
  size_t dispatched = 0u;
  voice_state_t *previous = s_polling;
  s_polling = state;
  int rc = h2_gizclaw_service_poll(state->service, 16u, &dispatched);
  s_polling = previous;
  return rc;
}

static int pump_pcm(voice_state_t *state);

static int step(voice_state_t *state) {
  int rc = pump_pcm(state);
  if (rc == H2_PAL_OK)
    rc = poll_service(state);
  if (rc == H2_PAL_OK)
    rc = atomic_load(&state->hook_error);
  if (rc == H2_PAL_OK)
    rc = h2_pal_time_sleep_ms(state->fixture->time, 1u);
  return rc;
}

static h2_pal_result_t read_pcm(void *user, uint8_t *out, size_t capacity,
                                size_t *out_len) {
  voice_state_t *state = user;
  if (state == NULL || out == NULL || out_len == NULL || capacity < 2u)
    return H2_PAL_ERR_INVALID_ARG;
  *out_len = 0u;
  atomic_fetch_add(&state->read_attempts, 1u);
  if (!atomic_load(&state->capture_enabled))
    return H2_PAL_ERR_WOULD_BLOCK;
  uint64_t now = 0u;
  int rc = clock_now(state, &now);
  if (rc != H2_PAL_OK)
    return rc;
  if (!state->capture_clock_started) {
    state->capture_clock_started = true;
    state->capture_started_ms = now;
  }
  if (now < state->capture_started_ms)
    return H2_PAL_ERR_INVALID_STATE;
  /* Pace actual PCM and VAD silence at 16 kHz mono, not CPU speed. */
  const uint64_t due_ms = state->emitted_bytes / 32u;
  if (now - state->capture_started_ms < due_ms)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (state->read_offset == state->fixture->pcm_len &&
      state->read_round + 1u < atomic_load(&state->clips_allowed)) {
    state->read_offset = 0u;
    ++state->read_round;
  }
  size_t len = capacity < FRAME_BYTES ? capacity : FRAME_BYTES;
  len &= ~(size_t)1u;
  if (state->read_offset < state->fixture->pcm_len) {
    const size_t left = state->fixture->pcm_len - state->read_offset;
    if (len > left)
      len = left;
    memcpy(out, state->fixture->pcm + state->read_offset, len);
    state->read_offset += len;
    state->mic_voice_len = len;
  } else if (state->realtime) {
    /* Keep RTP alive during model processing. Only the server chooses VAD
     * turn boundaries; the test does not send an EOS per utterance. */
    memset(out, 0, len);
    state->mic_voice_len = 0u;
  } else {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  /* Do not catch up a delayed pump by flooding multiple frames. */
  state->capture_started_ms = now - due_ms;
  state->emitted_bytes += len;
  *out_len = len;
  return H2_PAL_OK;
}

static h2_pal_result_t write_pcm(void *user, const uint8_t *pcm, size_t len) {
  voice_state_t *state = user;
  if (state == NULL || pcm == NULL || len == 0u || (len & 1u) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  atomic_fetch_add(&state->write_attempts, 1u);
  if (atomic_load(&state->block_playback))
    return H2_PAL_ERR_WOULD_BLOCK;
  for (size_t i = 0u; i < len; ++i)
    if (pcm[i] != 0u) {
      atomic_store(&state->non_silent, true);
      break;
    }
  atomic_fetch_add(&state->written, len);
  return H2_PAL_OK;
}

static h2_pal_result_t replacement_write(void *user, const uint8_t *pcm,
                                         size_t len) {
  voice_state_t *state = user;
  if (state == NULL || pcm == NULL || len == 0u || (len & 1u) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < len; ++i)
    if (pcm[i] != 0u) {
      atomic_store(&state->replacement_non_silent, true);
      break;
    }
  atomic_fetch_add(&state->replacement_written, len);
  return H2_PAL_OK;
}

/* These are external device pumps, not Track callbacks. GizClaw consumes and
 * produces the other ends independently on its audio tasks. */
static int pump_pcm(voice_state_t *state) {
  int rc = H2_PAL_OK;
  if (state->bound && atomic_load(&state->capture_enabled) &&
      atomic_load(&state->active)) {
    if (state->mic_pending_len == 0u)
      rc = read_pcm(state, state->mic_pending, sizeof(state->mic_pending),
                    &state->mic_pending_len);
    if (rc == H2_PAL_OK && state->mic_pending_len != 0u) {
      rc = h2_gizclaw_pcm_track_write(state->track, state->mic_pending,
                                      state->mic_pending_len);
      if (rc == H2_PAL_OK) {
        if (!state->mic_write_reported) {
          evidence("h2_gizclaw_pcm_track_write", "voice-pump", rc);
          state->mic_write_reported = true;
        }
        atomic_fetch_add(&state->captured, state->mic_voice_len);
        state->mic_pending_len = 0u;
      }
    }
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK)
      return rc;
  }
  if ((!state->bound && !state->replacement_bound) ||
      atomic_load(&state->block_playback))
    return H2_PAL_OK;
  uint64_t now = 0u;
  rc = clock_now(state, &now);
  if (rc != H2_PAL_OK || now < state->speaker_next_ms)
    return rc;
  h2_gizclaw_track_t *track =
      state->replacement_bound ? state->replacement_track : state->track;
  uint8_t pcm[FRAME_BYTES];
  size_t len = 0u;
  /* Exact reads preserve incomplete frames. A speaker may drain the final
   * partial frame without needing an EOS in the PCM ring. */
  while (len < sizeof(pcm)) {
    size_t wanted = len == 0u ? sizeof(pcm) : 2u;
    rc = h2_gizclaw_pcm_track_read(track, pcm + len, wanted);
    if (rc == H2_PAL_ERR_WOULD_BLOCK && wanted != 2u) {
      wanted = 2u;
      rc = h2_gizclaw_pcm_track_read(track, pcm + len, wanted);
    }
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      break;
    if (rc != H2_PAL_OK)
      return rc;
    len += wanted;
  }
  if (len != 0u) {
    if (!state->speaker_read_reported) {
      evidence("h2_gizclaw_pcm_track_read", "voice-pump", H2_PAL_OK);
      state->speaker_read_reported = true;
    }
    state->speaker_next_ms = now + 20u;
    return state->replacement_bound ? replacement_write(state, pcm, len)
                                    : write_pcm(state, pcm, len);
  }
  return H2_PAL_OK;
}

static int create_track(voice_state_t *state, h2_gizclaw_track_t **out) {
  const h2_gizclaw_pcm_track_config_t config = {.allocator =
                                                    state->fixture->allocator,
                                                .uplink_capacity = 4096u,
                                                .downlink_capacity = 1024u};
  return evidence("h2_gizclaw_pcm_track_create", "voice",
                  h2_gizclaw_pcm_track_create(&config, out));
}

/* Observable canaries for detached/canceled routes: fill uplink and empty
 * downlink. Any later successful read or write changes these boundaries. */
static int prime_idle_track(h2_gizclaw_track_t *track) {
  uint8_t sample[2] = {0x35, 0x72};
  int rc;
  for (size_t i = 0u; i <= 2048u; ++i) {
    rc = h2_gizclaw_pcm_track_write(track, sample, sizeof(sample));
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      break;
    if (rc != H2_PAL_OK || i == 2048u)
      return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : rc;
  }
  for (size_t i = 0u; i <= 512u; ++i) {
    rc = h2_gizclaw_pcm_track_read(track, sample, sizeof(sample));
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      return H2_PAL_OK;
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_ERR_INVALID_STATE;
}

static int check_idle_track(h2_gizclaw_track_t *track) {
  uint8_t sample[2] = {0};
  return h2_gizclaw_pcm_track_write(track, sample, sizeof(sample)) ==
                     H2_PAL_ERR_WOULD_BLOCK &&
                 h2_gizclaw_pcm_track_read(track, sample, sizeof(sample)) ==
                     H2_PAL_ERR_WOULD_BLOCK
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

static int drain_speaker(voice_state_t *state, size_t expected) {
  uint64_t started = 0u;
  int rc = clock_now(state, &started);
  while (rc == H2_PAL_OK && atomic_load(&state->written) < expected) {
    rc = within(state, started, HISTORY_TIMEOUT_MS);
    if (rc == H2_PAL_OK)
      rc = step(state);
  }
  return rc;
}

static int drain_completed_output(voice_state_t *state) {
  /* The producer has completed. At most 1024 bytes remain in this test's
   * Track, so two paced speaker frames drain the accepted tail. */
  for (unsigned i = 0u; i < 2u; ++i) {
    int rc = h2_pal_time_sleep_ms(state->fixture->time, 20u);
    if (rc == H2_PAL_OK)
      rc = pump_pcm(state);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t on_event(void *user,
                                h2_gizclaw_conversation_t *conversation,
                                const h2_gizclaw_conversation_event_t *event) {
  voice_state_t *state = user;
  if (s_polling != state || conversation != state->conversation ||
      event == NULL || event->generation != state->generation ||
      !atomic_load(&state->active)) {
    atomic_store(&state->hook_error, H2_PAL_ERR_INVALID_STATE);
    return H2_PAL_ERR_INVALID_STATE;
  }
  int rc = H2_PAL_OK;
  switch (event->kind) {
  case H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA:
  case H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE:
    if (event->text == NULL ||
        memchr(event->text, '\0', event->text_len) != NULL)
      rc = H2_PAL_ERR_FORMAT;
    if (rc == H2_PAL_OK && event->text_len != 0u)
      state->round_text_seen = true;
    if (rc == H2_PAL_OK &&
        event->kind == H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE)
      state->round_text_done = true;
    break;
  case H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO:
    if (event->audio == NULL || event->audio_len == 0u ||
        (event->audio_len & 1u) != 0u) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    state->round_audio += event->audio_len;
    state->hook_audio_total += event->audio_len;
    for (size_t i = 0u; i < event->audio_len; ++i)
      state->round_non_silent |= event->audio[i] != 0u;
    break;
  case H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE:
    if (!state->round_text_seen || !state->round_text_done ||
        !state->round_non_silent || state->round_audio == 0u)
      rc = H2_PAL_ERR_INVALID_STATE;
    if (rc == H2_PAL_OK)
      atomic_fetch_add(&state->rounds, 1u);
    state->round_text_seen = state->round_text_done = state->round_non_silent =
        false;
    state->round_audio = 0u;
    break;
  case H2_GIZCLAW_CONVERSATION_EVENT_ERROR:
    rc = H2_GIZCLAW_ERR_REMOTE;
    break;
  case H2_GIZCLAW_CONVERSATION_EVENT_NONE:
  default:
    rc = H2_PAL_ERR_INVALID_STATE;
    break;
  }
  if (rc != H2_PAL_OK)
    atomic_store(&state->hook_error, rc);
  return rc;
}

static void on_complete(void *user, h2_gizclaw_conversation_t *conversation,
                        const h2_gizclaw_operation_result_t *result) {
  voice_state_t *state = user;
  if (s_polling != state || conversation != state->conversation ||
      result == NULL || result->identity != state->generation ||
      !atomic_load(&state->active))
    atomic_store(&state->hook_error, H2_PAL_ERR_INVALID_STATE);
  if (result != NULL) {
    atomic_store(&state->terminal_result, result->result);
    atomic_store(&state->terminal_kind, result->terminal_kind);
  }
  atomic_fetch_add(&state->completions, 1u);
  atomic_store(&state->active, false);
}

static void reset_capture(voice_state_t *state, bool realtime) {
  state->realtime = realtime;
  state->capture_clock_started = false;
  state->emitted_bytes = state->read_offset = state->read_round = 0u;
  state->mic_pending_len = state->mic_voice_len = 0u;
  state->round_text_seen = state->round_text_done = state->round_non_silent =
      false;
  state->round_audio = 0u;
  state->hook_audio_total = 0u;
  atomic_store(&state->clips_allowed, 1u);
  atomic_store(&state->captured, 0u);
  atomic_store(&state->written, 0u);
  atomic_store(&state->write_attempts, 0u);
  atomic_store(&state->non_silent, false);
  atomic_store(&state->hook_error, H2_PAL_OK);
  atomic_store(&state->completions, 0u);
  atomic_store(&state->rounds, 0u);
  atomic_store(&state->capture_enabled, true);
}

static int begin(voice_state_t *state) {
  ++state->generation;
  atomic_store(&state->active, true);
  int rc = evidence("h2_gizclaw_service_audio_start", "voice",
                    h2_gizclaw_service_audio_start(state->service));
  if (rc != H2_PAL_OK)
    atomic_store(&state->active, false);
  return rc;
}

static int end_input(voice_state_t *state) {
  int rc = evidence("h2_gizclaw_service_audio_end", "voice",
                    h2_gizclaw_service_audio_end(state->service));
  if (rc == H2_PAL_OK)
    rc = evidence("h2_gizclaw_service_audio_end", "voice-repeat-end",
                  h2_gizclaw_service_audio_end(state->service));
  if (rc == H2_PAL_OK)
    atomic_store(&state->capture_enabled, false);
  return rc;
}

static int configure_mode(voice_state_t *state, bool realtime) {
  state->storage.used = 0u;
  h2_gizclaw_workspace_t workspace = {0};
  int rc = h2_gizclaw_rpc_workspace_set_input(
      state->service, h2_gizclaw_e2e_str(state->workspace_name),
      realtime ? H2_GIZCLAW_WORKSPACE_INPUT_REALTIME
               : H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK,
      30000u, &state->storage, &workspace);
  if (rc == H2_PAL_OK && (!workspace.available ||
                          !response_text(&state->storage, workspace.name, true,
                                         H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES) ||
                          strcmp(workspace.name, state->workspace_name) != 0))
    rc = H2_PAL_ERR_FORMAT;
  state->storage.used = 0u;
  h2_gizclaw_workspace_activation_t activation = {0};
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_rpc_workspace_activate(
        state->service, h2_gizclaw_e2e_str(state->workspace_name), 30000u,
        &state->storage, &activation);
  if (rc == H2_PAL_OK &&
      (!response_text(&state->storage, activation.workspace_name, true,
                      H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES) ||
       strcmp(activation.workspace_name, state->workspace_name) != 0))
    rc = H2_PAL_ERR_FORMAT;
  state->storage.used = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_rpc_workspace_reload(state->service, 30000u,
                                         &state->storage, &activation);
  if (rc == H2_PAL_OK &&
      (!response_text(&state->storage, activation.active_workspace_name, true,
                      H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES) ||
       strcmp(activation.active_workspace_name, state->workspace_name) != 0 ||
       activation.runtime_state != H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING))
    rc = H2_PAL_ERR_FORMAT;
  state->storage.used = 0u;
  return rc;
}

static int conversation_rounds(voice_state_t *state, bool realtime) {
  int rc = configure_mode(state, realtime);
  if (rc != H2_PAL_OK)
    return rc;
  reset_capture(state, realtime);
  uint64_t started = 0u;
  rc = clock_now(state, &started);
  if (rc == H2_PAL_OK)
    rc = begin(state);
  bool ended = false;
  uint64_t hangup_started = 0u;
  while (rc == H2_PAL_OK && atomic_load(&state->active)) {
    rc = within(state, realtime && ended ? hangup_started : started,
                realtime && ended ? DISPOSE_TIMEOUT_MS : VOICE_TIMEOUT_MS);
    if (rc != H2_PAL_OK)
      break;
    if (realtime && atomic_load(&state->rounds) == 1u)
      atomic_store(&state->clips_allowed, 2u);
    const bool ready =
        realtime ? atomic_load(&state->rounds) == 2u
                 : atomic_load(&state->captured) == state->fixture->pcm_len;
    if (!ended && ready) {
      if (realtime) {
        /* Telephone semantics: the caller hangs up the active stream. Reply
         * EOS only delimits VAD rounds; do not wait for another server reply
         * or invent a session-completion acknowledgement after two rounds. */
        atomic_store(&state->capture_enabled, false);
        rc = clock_now(state, &hangup_started);
        if (rc == H2_PAL_OK)
          rc = evidence("h2_gizclaw_conversation_cancel", "voice-hangup",
                        h2_gizclaw_conversation_cancel(state->conversation));
      } else {
        rc = end_input(state);
      }
      ended = rc == H2_PAL_OK;
    }
    if (rc == H2_PAL_OK)
      rc = step(state);
  }
  atomic_store(&state->capture_enabled, false);
  /* PTT plays every announced chunk to the end. A realtime hangup discards
   * whatever the Track still queued at cancel, so the speaker may finish a
   * frame or two short of the hook total but never ahead of it. */
  if (rc == H2_PAL_OK)
    rc = realtime ? drain_completed_output(state)
                  : drain_speaker(state, state->hook_audio_total);
  const size_t written = atomic_load(&state->written);
  const bool playback_matches =
      realtime ? written <= state->hook_audio_total &&
                     written + 2u * FRAME_BYTES >= state->hook_audio_total
               : written == state->hook_audio_total;
  if (rc == H2_PAL_OK &&
      (!ended || atomic_load(&state->completions) != 1u ||
       atomic_load(&state->terminal_kind) !=
           (realtime ? H2_GIZCLAW_OPERATION_CANCELED
                     : H2_GIZCLAW_OPERATION_FINISHED) ||
       atomic_load(&state->rounds) != (realtime ? 2u : 1u) ||
       atomic_load(&state->captured) !=
           state->fixture->pcm_len * (realtime ? 2u : 1u) ||
       !playback_matches || !atomic_load(&state->non_silent) || written == 0u))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    const int terminal = atomic_load(&state->terminal_result);
    const int expected = realtime ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
    rc = terminal == expected    ? H2_PAL_OK
         : terminal == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE
                                 : terminal;
  }
  evidence("h2_gizclaw_service_audio_start", "service_audio_start-assert", rc);
  evidence(realtime ? "h2_gizclaw_conversation_cancel"
                    : "h2_gizclaw_service_audio_end",
           realtime ? "conversation_cancel-assert" : "service_audio_end-assert",
           rc);
  printf("H2_GIZCLAW_E2E stage=voice mode=%s result=%s rc=%d rounds=%u "
         "capture_bytes=%zu playback_bytes=%zu hook_bytes=%zu\n",
         realtime ? "realtime-vad" : "ptt", rc == H2_PAL_OK ? "PASS" : "FAIL",
         rc, atomic_load(&state->rounds), atomic_load(&state->captured),
         atomic_load(&state->written), state->hook_audio_total);
  return rc;
}

static int cancel_conversation(voice_state_t *state) {
  reset_capture(state, false);
  int rc = begin(state);
  uint64_t started = 0u;
  if (rc == H2_PAL_OK)
    rc = clock_now(state, &started);
  while (rc == H2_PAL_OK && atomic_load(&state->captured) == 0u) {
    rc = within(state, started, DISPOSE_TIMEOUT_MS);
    if (rc == H2_PAL_OK)
      rc = step(state);
  }
  if (rc == H2_PAL_OK)
    rc = evidence("h2_gizclaw_conversation_cancel", "voice-cancel",
                  h2_gizclaw_conversation_cancel(state->conversation));
  while (rc == H2_PAL_OK && atomic_load(&state->active)) {
    rc = within(state, started, DISPOSE_TIMEOUT_MS);
    if (rc == H2_PAL_OK)
      rc = step(state);
  }
  if (rc == H2_PAL_OK &&
      (atomic_load(&state->completions) != 1u ||
       atomic_load(&state->terminal_kind) != H2_GIZCLAW_OPERATION_CANCELED ||
       atomic_load(&state->terminal_result) != H2_PAL_ERR_CLOSED))
    rc = H2_PAL_ERR_INVALID_STATE;
  atomic_store(&state->capture_enabled, false);
  if (rc == H2_PAL_OK) {
    rc = prime_idle_track(state->track);
    if (rc == H2_PAL_OK)
      rc = h2_pal_time_sleep_ms(state->fixture->time, 50u);
    if (rc == H2_PAL_OK)
      rc = poll_service(state);
    if (rc == H2_PAL_OK)
      rc = check_idle_track(state->track);
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_service_unset_track(state->service, state->track);
    if (rc == H2_PAL_OK) {
      state->bound = false;
      rc = h2_gizclaw_pcm_track_destroy(&state->track);
    }
    if (rc == H2_PAL_OK)
      rc = create_track(state, &state->track);
    if (rc == H2_PAL_OK) {
      rc = h2_gizclaw_service_set_track(state->service, state->track);
      state->bound = rc == H2_PAL_OK;
    }
  }
  return evidence("h2_gizclaw_conversation_cancel",
                  "conversation_cancel-assert", rc);
}

static bool snapshot_contains(const history_snapshot_t *snapshot,
                              const char *id) {
  for (size_t i = 0u; i < snapshot->count; ++i)
    if (strcmp(snapshot->ids[i], id) == 0)
      return true;
  return false;
}

static int history_page(voice_state_t *state,
                        h2_gizclaw_workspace_history_page_t *page) {
  state->storage.used = 0u;
  int rc = h2_gizclaw_rpc_workspace_history_list(
      state->service, h2_gizclaw_e2e_str(state->workspace_name),
      (h2_gizclaw_str_t){0}, H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS,
      H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC, 30000u, &state->storage, page);
  if (rc != H2_PAL_OK)
    return rc;
  if (state->storage.used > state->storage.capacity ||
      page->count > H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS ||
      (page->count != 0u &&
       (!response_owns(&state->storage, page->items,
                       page->count * sizeof(*page->items)) ||
        (uintptr_t)page->items %
                _Alignof(h2_gizclaw_workspace_history_entry_t) !=
            0u)) ||
      !response_text(&state->storage, page->next_cursor, page->has_next,
                     SIZE_MAX))
    return H2_PAL_ERR_FORMAT;
  if (!page->available)
    return H2_PAL_ERR_INVALID_STATE;
  for (size_t i = 0u; i < page->count; ++i) {
    const h2_gizclaw_workspace_history_entry_t *entry = &page->items[i];
    if (!response_text(&state->storage, entry->id, true,
                       H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES) ||
        !response_text(&state->storage, entry->text, false,
                       H2_GIZCLAW_WORKSPACE_HISTORY_TEXT_MAX_BYTES))
      return H2_PAL_ERR_FORMAT;
    for (size_t j = 0u; j < i; ++j)
      if (strcmp(entry->id, page->items[j].id) == 0)
        return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

static int capture_history(voice_state_t *state, history_snapshot_t *snapshot) {
  h2_gizclaw_workspace_history_page_t page = {0};
  int rc = history_page(state, &page);
  snapshot->count = 0u;
  /* A partial baseline could label an older, unseen reply as newly generated.
   * This isolated Voice fixture retains at most one full page of IDs. */
  if (rc == H2_PAL_OK && page.has_next)
    rc = H2_PAL_ERR_NO_SPACE;
  if (rc == H2_PAL_OK)
    for (size_t i = 0u; i < page.count; ++i) {
      const char *id = page.items[i].id;
      /* history_page validated every ID before publishing any snapshot. */
      memcpy(snapshot->ids[snapshot->count++], id, strlen(id) + 1u);
    }
  state->storage.used = 0u;
  return rc;
}

static int new_history(voice_state_t *state, char *id) {
  uint64_t started = 0u;
  int rc = clock_now(state, &started);
  while (rc == H2_PAL_OK) {
    rc = within(state, started, HISTORY_TIMEOUT_MS);
    h2_gizclaw_workspace_history_page_t page = {0};
    if (rc == H2_PAL_OK)
      rc = history_page(state, &page);
    if (rc != H2_PAL_OK)
      break;
    bool found = false;
    for (size_t i = 0u; i < page.count; ++i) {
      const h2_gizclaw_workspace_history_entry_t *entry = &page.items[i];
      if (snapshot_contains(&state->before, entry->id) ||
          entry->type != H2_GIZCLAW_WORKSPACE_HISTORY_AGENT ||
          !entry->replay_available || entry->text == NULL ||
          entry->text[0] == '\0')
        continue;
      const size_t len = strlen(entry->id);
      memcpy(id, entry->id, len + 1u);
      found = true;
      break;
    }
    state->storage.used = 0u;
    if (rc != H2_PAL_OK || found)
      break;
    rc = h2_pal_time_sleep_ms(state->fixture->time, 50u);
  }
  return rc;
}

static int play_history(voice_state_t *state, const char *id, bool cancel) {
  atomic_store(&state->written, 0u);
  atomic_store(&state->write_attempts, 0u);
  atomic_store(&state->non_silent, false);
  atomic_store(&state->block_playback, cancel);
  int rc = evidence("h2_gizclaw_req_create_audio_play", "history-play",
                    h2_gizclaw_req_create_audio_play(
                        state->service, cancel ? 51u : 50u,
                        h2_gizclaw_e2e_str(state->fixture->workspace_name),
                        h2_gizclaw_e2e_str(id), 30000u, &state->play));
  if (rc == H2_PAL_OK)
    rc = evidence("h2_gizclaw_req_do", "history-play",
                  h2_gizclaw_req_do(state->play, NULL, NULL, NULL, NULL));
  uint64_t started = 0u;
  if (rc == H2_PAL_OK)
    rc = clock_now(state, &started);
  if (cancel) {
    /* Keep the speaker stopped briefly, then cancel without depending on
     * whether the complete reply fits in the bounded PCM ring. */
    while (rc == H2_PAL_OK) {
      rc = within(state, started, HISTORY_TIMEOUT_MS);
      if (rc != H2_PAL_OK)
        break;
      int wait_rc = h2_gizclaw_req_wait(state->play, 10u);
      if (wait_rc != H2_PAL_ERR_TIMEOUT) {
        rc = wait_rc;
        break;
      }
      uint64_t now = 0u;
      rc = clock_now(state, &now);
      if (rc != H2_PAL_OK || now - started >= 100u)
        break;
      rc = h2_pal_time_sleep_ms(state->fixture->time, 1u);
    }
    bool already_complete = false;
    if (rc == H2_PAL_OK) {
      const int wait_rc = h2_gizclaw_req_wait(state->play, 0u);
      already_complete = wait_rc == H2_PAL_OK;
      if (wait_rc != H2_PAL_OK && wait_rc != H2_PAL_ERR_TIMEOUT)
        rc = wait_rc;
    }
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_req_cancel(state->play);
    if (rc == H2_PAL_OK) {
      int wait_rc = h2_gizclaw_req_wait(state->play, DISPOSE_TIMEOUT_MS);
      rc = wait_rc == H2_PAL_ERR_CLOSED ||
                   (already_complete && wait_rc == H2_PAL_OK)
               ? H2_PAL_OK
               : (wait_rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : wait_rc);
    }
    if (rc == H2_PAL_OK) {
      const size_t attempts = atomic_load(&state->write_attempts);
      atomic_store(&state->block_playback, false);
      rc = h2_pal_time_sleep_ms(state->fixture->time, 50u);
      if (rc == H2_PAL_OK && (atomic_load(&state->written) != 0u ||
                              attempts != atomic_load(&state->write_attempts)))
        rc = H2_PAL_ERR_INVALID_STATE;
      /* Cancel does not rewind the producer cursor. The speaker owns removal
       * of samples already delivered to the Track. */
      uint8_t sample[2];
      for (size_t i = 0u; rc == H2_PAL_OK && i <= 512u; ++i) {
        int read_rc = h2_gizclaw_pcm_track_read(state->track, sample, 2u);
        if (read_rc == H2_PAL_ERR_WOULD_BLOCK)
          break;
        rc = read_rc == H2_PAL_OK && i == 512u ? H2_PAL_ERR_INVALID_STATE
                                               : read_rc;
      }
    }
  } else {
    while (rc == H2_PAL_OK) {
      rc = h2_gizclaw_req_wait(state->play, 10u);
      if (rc == H2_PAL_OK)
        break;
      if (rc != H2_PAL_ERR_TIMEOUT)
        return rc;
      rc = within(state, started, HISTORY_TIMEOUT_MS);
      if (rc == H2_PAL_OK)
        rc = step(state);
    }
    evidence("h2_gizclaw_req_wait", "history-play", rc);
    if (rc == H2_PAL_OK)
      rc = drain_completed_output(state);
    if (rc == H2_PAL_OK && (atomic_load(&state->written) == 0u ||
                            !atomic_load(&state->non_silent)))
      rc = H2_PAL_ERR_INVALID_STATE;
    evidence("h2_gizclaw_req_create_audio_play", "audio_play-assert", rc);
  }
  printf("H2_GIZCLAW_E2E stage=history-play cancel=%s result=%s rc=%d "
         "pcm_bytes=%zu\n",
         cancel ? "true" : "false", rc == H2_PAL_OK ? "PASS" : "FAIL", rc,
         atomic_load(&state->written));
  if (rc != H2_PAL_OK)
    return rc; /* cleanup retains the request and Track */
  h2_gizclaw_req_release(state->play);
  state->play = NULL;
  return H2_PAL_OK;
}

static int dispose_voice(h2_gizclaw_e2e_fixture_t *fixture) {
  voice_state_t *state = fixture->case_state;
  if (state == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  atomic_store(&state->capture_enabled, false);
  if (state->play != NULL) {
    (void)h2_gizclaw_req_cancel(state->play);
    h2_gizclaw_req_release(state->play);
    state->play = NULL;
  }
  int rc = H2_PAL_OK;
  int terminal_rc = H2_PAL_OK;
  if (state->conversation != NULL && atomic_load(&state->active)) {
    const unsigned completions = atomic_load(&state->completions);
    rc = h2_gizclaw_conversation_cancel(state->conversation);
    uint64_t started = 0u;
    if (rc == H2_PAL_OK)
      rc = clock_now(state, &started);
    while (rc == H2_PAL_OK && atomic_load(&state->active)) {
      /* A stopped publisher may already have queued completion, even if the
       * case deadline expired. Drain before deciding whether to keep waiting.
       */
      rc = poll_service(state);
      if (rc == H2_PAL_OK && atomic_load(&state->active))
        rc = within(state, started, DISPOSE_TIMEOUT_MS);
      if (rc == H2_PAL_OK && atomic_load(&state->active))
        rc = h2_pal_time_sleep_ms(fixture->time, 1u);
    }
    if (rc == H2_PAL_OK) {
      if (atomic_load(&state->completions) != completions + 1u ||
          atomic_load(&state->terminal_kind) != H2_GIZCLAW_OPERATION_CANCELED)
        terminal_rc = H2_PAL_ERR_INVALID_STATE;
      else if (atomic_load(&state->terminal_result) != H2_PAL_ERR_CLOSED)
        terminal_rc = atomic_load(&state->terminal_result) == H2_PAL_OK
                          ? H2_PAL_ERR_INVALID_STATE
                          : atomic_load(&state->terminal_result);
      if (terminal_rc == H2_PAL_OK)
        terminal_rc = atomic_load(&state->hook_error);
    }
  }
  if (rc != H2_PAL_OK)
    return rc;
  if (state->conversation != NULL) {
    h2_gizclaw_conversation_release(state->conversation);
    evidence("h2_gizclaw_conversation_release", "voice-cleanup", H2_PAL_OK);
    state->conversation = NULL;
  }
  if (state->bound) {
    rc = evidence("h2_gizclaw_service_unset_track", "voice-cleanup",
                  h2_gizclaw_service_unset_track(state->service, state->track));
    if (rc != H2_PAL_OK)
      return rc;
    state->bound = false;
  }
  if (state->replacement_bound) {
    rc = evidence("h2_gizclaw_service_unset_track", "voice-cleanup",
                  h2_gizclaw_service_unset_track(state->service,
                                                 state->replacement_track));
    if (rc != H2_PAL_OK)
      return rc;
    state->replacement_bound = false;
  }
  rc = evidence("h2_gizclaw_pcm_track_destroy", "voice-cleanup",
                h2_gizclaw_pcm_track_destroy(&state->track));
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_pcm_track_destroy(&state->replacement_track);
  if (rc != H2_PAL_OK)
    return rc;
  fixture->case_state = NULL;
  fixture->case_cleanup = NULL;
  h2_pal_mem_free(fixture->allocator, state->response);
  h2_pal_mem_free(fixture->allocator, state);
  return terminal_rc;
}

/* Keep the old Track and user alive while real playback exercises a different
 * sink on the same Service. Neither callbacks that return WOULD_BLOCK nor
 * successful stale writes may touch the detached Track during this probe. */
static int verify_track_replacement(voice_state_t *state, const char *id) {
  if (!h2_gizclaw_e2e_fixture_has_time(state->fixture, HISTORY_TIMEOUT_MS))
    return H2_PAL_ERR_TIMEOUT;
  int rc =
      evidence("h2_gizclaw_service_unset_track", "voice-replace",
               h2_gizclaw_service_unset_track(state->service, state->track));
  if (rc != H2_PAL_OK)
    return rc;
  state->bound = false;
  const size_t reads = atomic_load(&state->read_attempts);
  const size_t writes = atomic_load(&state->write_attempts);
  rc = prime_idle_track(state->track);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_gizclaw_pcm_track_config_t config = {.allocator =
                                                    state->fixture->allocator,
                                                .uplink_capacity = 4096u,
                                                .downlink_capacity = 1024u};
  rc = h2_gizclaw_pcm_track_create(&config, &state->replacement_track);
  if (rc != H2_PAL_OK)
    return rc;
  rc = evidence(
      "h2_gizclaw_service_set_track", "voice-replace",
      h2_gizclaw_service_set_track(state->service, state->replacement_track));
  if (rc != H2_PAL_OK)
    return rc;
  state->replacement_bound = true;
  rc = evidence("h2_gizclaw_req_create_audio_play", "voice-replace",
                h2_gizclaw_req_create_audio_play(
                    state->service, 52u,
                    h2_gizclaw_e2e_str(state->fixture->workspace_name),
                    h2_gizclaw_e2e_str(id), HISTORY_TIMEOUT_MS, &state->play));
  if (rc == H2_PAL_OK)
    rc = evidence("h2_gizclaw_req_do", "voice-replace",
                  h2_gizclaw_req_do(state->play, NULL, NULL, NULL, NULL));
  uint64_t started = 0u;
  if (rc == H2_PAL_OK)
    rc = clock_now(state, &started);
  while (rc == H2_PAL_OK) {
    rc = h2_gizclaw_req_wait(state->play, 10u);
    if (rc == H2_PAL_OK)
      break;
    if (rc != H2_PAL_ERR_TIMEOUT)
      return rc;
    rc = within(state, started, HISTORY_TIMEOUT_MS);
    if (rc == H2_PAL_OK)
      rc = step(state);
  }
  if (rc == H2_PAL_OK)
    rc = drain_completed_output(state);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_req_release(state->play);
  state->play = NULL;
  rc = evidence(
      "h2_gizclaw_service_unset_track", "voice-replace",
      h2_gizclaw_service_unset_track(state->service, state->replacement_track));
  if (rc != H2_PAL_OK)
    return rc;
  state->replacement_bound = false;
  if (atomic_load(&state->replacement_written) == 0u ||
      !atomic_load(&state->replacement_non_silent) ||
      reads != atomic_load(&state->read_attempts) ||
      writes != atomic_load(&state->write_attempts))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    rc = check_idle_track(state->track);
  return evidence("h2_gizclaw_service_unset_track",
                  "service_unset_track-assert", rc);
}

/* An SFU Workspace is a walkie-talkie: the runtime forwards the sender's
 * utterance to the other members and keeps no History, and the sender's own
 * route receives neither a reply nor a terminal, so the turn never finishes by
 * itself. A rejected turn (SFU_RUNTIME_NOT_ATTACHED, SFU_ACCESS_REVOKED,
 * SFU_ACCESS_CHECK_FAILED) arrives as a typed EOS error on the same stream and
 * surfaces here as CONVERSATION_EVENT_ERROR. Acceptance is therefore a turn
 * that stays open and silent through the grace window after EOS; dispose_voice
 * then hangs up and requires the CANCELED terminal. */
static int talk_group_clip(voice_state_t *state) {
  int rc = configure_mode(state, false);
  if (rc != H2_PAL_OK)
    return rc;
  reset_capture(state, false);
  uint64_t started = 0u;
  rc = clock_now(state, &started);
  if (rc == H2_PAL_OK)
    rc = begin(state);
  while (rc == H2_PAL_OK &&
         atomic_load(&state->captured) < state->fixture->pcm_len) {
    rc = within(state, started, VOICE_TIMEOUT_MS);
    if (rc == H2_PAL_OK)
      rc = step(state);
    if (rc == H2_PAL_OK && !atomic_load(&state->active))
      rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK)
    rc = end_input(state);
  uint64_t ended = 0u;
  if (rc == H2_PAL_OK)
    rc = clock_now(state, &ended);
  while (rc == H2_PAL_OK) {
    uint64_t now = 0u;
    rc = clock_now(state, &now);
    if (rc != H2_PAL_OK || now - ended >= GROUP_TALK_GRACE_MS)
      break;
    rc = within(state, started, VOICE_TIMEOUT_MS);
    if (rc == H2_PAL_OK)
      rc = step(state);
    if (rc == H2_PAL_OK && !atomic_load(&state->active)) {
      /* The Server ended the turn: report its own result, or the unexpected
       * finish of a route that must stay open until the local hangup. */
      const int terminal = atomic_load(&state->terminal_result);
      rc = terminal != H2_PAL_OK ? terminal : H2_PAL_ERR_INVALID_STATE;
    }
  }
  /* Half-duplex: the speaker never hears its own utterance back. */
  if (rc == H2_PAL_OK &&
      (atomic_load(&state->completions) != 0u ||
       atomic_load(&state->rounds) != 0u || atomic_load(&state->written) != 0u))
    rc = H2_PAL_ERR_INVALID_STATE;
  return rc;
}

static int run_voice(h2_gizclaw_e2e_fixture_t *fixture, bool group_talk) {
  if (fixture == NULL || fixture->pcm == NULL || fixture->pcm_len == 0u ||
      (fixture->pcm_len & 1u) != 0u || fixture->pcm_len > SIZE_MAX / 2u ||
      !(group_talk ? fixture->friend_group_created
                   : fixture->workspace_created) ||
      fixture->actors[0].service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const char *workspace_name = group_talk ? fixture->friend_group_workspace_name
                                          : fixture->workspace_name;
  if (workspace_name[0] == '\0' ||
      memchr(workspace_name, '\0', H2_GIZCLAW_E2E_NAME_CAPACITY) == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (fixture->case_state != NULL || fixture->case_cleanup != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  voice_state_t *state = h2_pal_mem_alloc(fixture->allocator, sizeof(*state));
  if (state == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(state, 0, sizeof(*state));
  state->fixture = fixture;
  state->service = fixture->actors[0].service;
  state->workspace_name = workspace_name;
  state->group_talk = group_talk;
  atomic_init(&state->clips_allowed, 0u);
  atomic_init(&state->capture_enabled, false);
  atomic_init(&state->block_playback, false);
  atomic_init(&state->active, false);
  atomic_init(&state->captured, 0u);
  atomic_init(&state->written, 0u);
  atomic_init(&state->write_attempts, 0u);
  atomic_init(&state->read_attempts, 0u);
  atomic_init(&state->replacement_written, 0u);
  atomic_init(&state->replacement_non_silent, false);
  atomic_init(&state->non_silent, false);
  atomic_init(&state->hook_error, H2_PAL_OK);
  atomic_init(&state->terminal_result, H2_PAL_OK);
  atomic_init(&state->terminal_kind, H2_GIZCLAW_OPERATION_FINISHED);
  atomic_init(&state->completions, 0u);
  atomic_init(&state->rounds, 0u);
  fixture->case_state = state;
  fixture->case_cleanup = dispose_voice;
  state->response = h2_pal_mem_alloc(fixture->allocator, RESPONSE_BYTES);
  state->storage = (h2_gizclaw_resp_storage_t){.data = state->response,
                                               .capacity = RESPONSE_BYTES};
  const h2_gizclaw_pcm_track_config_t config = {.allocator = fixture->allocator,
                                                .uplink_capacity = 4096u,
                                                .downlink_capacity = 1024u};
  /* An SFU Workspace owns no History; only the Workflow Workspace keeps a
   * baseline for the new-reply search. */
  int rc = state->response == NULL ? H2_PAL_ERR_NO_MEMORY
           : group_talk            ? H2_PAL_OK
                                   : capture_history(state, &state->before);
  if (rc == H2_PAL_OK)
    rc = evidence("h2_gizclaw_pcm_track_create", "voice",
                  h2_gizclaw_pcm_track_create(&config, &state->track));
  if (rc == H2_PAL_OK) {
    rc = evidence("h2_gizclaw_service_set_track", "voice",
                  h2_gizclaw_service_set_track(state->service, state->track));
    state->bound = rc == H2_PAL_OK;
  }
  if (rc == H2_PAL_OK)
    rc = evidence("h2_gizclaw_conversation_create", "voice",
                  h2_gizclaw_conversation_create(
                      state->service, h2_gizclaw_e2e_str(state->workspace_name),
                      on_event, on_complete, state, &state->conversation));
  if (rc == H2_PAL_OK) {
    rc = group_talk ? talk_group_clip(state)
                    : conversation_rounds(state, false);
    evidence("h2_gizclaw_service_set_track", "service_set_track-assert", rc);
    evidence("h2_gizclaw_conversation_create", "conversation_create-assert",
             rc);
  }
  if (group_talk) {
    const size_t captured = atomic_load(&state->captured);
    const int cleanup_rc = dispose_voice(fixture);
    if (rc == H2_PAL_OK)
      rc = cleanup_rc;
    printf("H2_GIZCLAW_E2E stage=group-talk result=%s rc=%d "
           "capture_bytes=%zu\n",
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc, captured);
    return rc;
  }
  char history_id[H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES + 1u] = {0};
  if (rc == H2_PAL_OK)
    rc = new_history(state, history_id);
  if (rc == H2_PAL_OK)
    rc = play_history(state, history_id, false);
  if (rc == H2_PAL_OK)
    rc = play_history(state, history_id, true);
  if (rc == H2_PAL_OK)
    rc = cancel_conversation(state);
  if (rc == H2_PAL_OK) {
    h2_gizclaw_ping_result_t ping = {0};
    rc = h2_gizclaw_rpc_ping(state->service, 30000u, &ping);
  }
  if (rc == H2_PAL_OK)
    rc = conversation_rounds(state, true);
  if (rc == H2_PAL_OK) {
    h2_gizclaw_ping_result_t ping = {0};
    rc = h2_gizclaw_rpc_ping(state->service, 30000u, &ping);
    if (rc == H2_PAL_OK && ping.server_time_ms <= 0)
      rc = H2_PAL_ERR_FORMAT;
    evidence("h2_gizclaw_rpc_ping", "voice-hangup-peer-alive", rc);
  }
  if (rc == H2_PAL_OK)
    rc = verify_track_replacement(state, history_id);
  const int cleanup_rc = dispose_voice(fixture);
  if (rc == H2_PAL_OK)
    rc = cleanup_rc;
  if (rc == H2_PAL_OK) {
    if (fixture->case_state != NULL || fixture->case_cleanup != NULL)
      rc = H2_PAL_ERR_INVALID_STATE;
    evidence("h2_gizclaw_conversation_release", "conversation_release-assert",
             rc);
  }
  /* Only reconnect once all local hooks/Track borrows have ended. */
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_e2e_fixture_reconnect_actor(fixture, H2_GIZCLAW_E2E_OWNER);
  if (rc == H2_PAL_OK) {
    state = h2_pal_mem_alloc(fixture->allocator, sizeof(*state));
    if (state == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    memset(state, 0, sizeof(*state));
    state->fixture = fixture;
    state->service = fixture->actors[0].service;
    state->workspace_name = workspace_name;
    state->response = h2_pal_mem_alloc(fixture->allocator, RESPONSE_BYTES);
    state->storage = (h2_gizclaw_resp_storage_t){.data = state->response,
                                                 .capacity = RESPONSE_BYTES};
    rc = state->response == NULL ? H2_PAL_ERR_NO_MEMORY
                                 : capture_history(state, &state->before);
    if (rc == H2_PAL_OK && !snapshot_contains(&state->before, history_id))
      rc = H2_PAL_ERR_NOT_FOUND;
    h2_pal_mem_free(fixture->allocator, state->response);
    h2_pal_mem_free(fixture->allocator, state);
  }
  if (rc == H2_PAL_OK) {
    evidence("h2_gizclaw_pcm_track_create", "pcm_track_create-assert", rc);
    evidence("h2_gizclaw_pcm_track_write", "pcm_track_write-assert", rc);
    evidence("h2_gizclaw_pcm_track_read", "pcm_track_read-assert", rc);
    evidence("h2_gizclaw_pcm_track_destroy", "pcm_track_destroy-assert", rc);
  }
  return rc;
}

int h2_gizclaw_e2e_run_voice(h2_gizclaw_e2e_fixture_t *fixture) {
  return run_voice(fixture, false);
}

int h2_gizclaw_e2e_run_group_talk(h2_gizclaw_e2e_fixture_t *fixture) {
  return run_voice(fixture, true);
}
