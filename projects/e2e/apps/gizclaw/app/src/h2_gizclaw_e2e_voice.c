#include "h2_gizclaw_e2e_voice.h"

#include "opus.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define H2_GIZCLAW_E2E_PCM_RATE_HZ 16000u
#define H2_GIZCLAW_E2E_PCM_FRAME_SAMPLES 320u
#define H2_GIZCLAW_E2E_REPLY_RATE_HZ 48000u
#define H2_GIZCLAW_E2E_REPLY_MAX_SAMPLES 5760
#define H2_GIZCLAW_E2E_RESPONSE_TIMEOUT_MS 90000u
#define H2_GIZCLAW_E2E_HISTORY_TIMEOUT_MS 30000u
#define H2_GIZCLAW_E2E_HISTORY_MAX_BYTES (16u * 1024u * 1024u)

typedef struct history_snapshot {
  char ids[H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS]
          [H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES + 1u];
  size_t count;
} history_snapshot_t;

typedef struct byte_buffer {
  const h2_pal_mem_api_t *allocator;
  uint8_t *data;
  size_t len;
  size_t capacity;
} byte_buffer_t;

typedef struct ogg_cursor {
  const uint8_t *data;
  size_t size;
  size_t page_offset;
  const uint8_t *segments;
  size_t segment_count;
  size_t segment_index;
  const uint8_t *payload;
  size_t payload_left;
  uint8_t *packet;
  size_t packet_capacity;
  size_t packet_len;
} ogg_cursor_t;

static bool snapshot_contains(const history_snapshot_t *snapshot,
                              const char *id) {
  for (size_t index = 0u; index < snapshot->count; ++index) {
    if (strcmp(snapshot->ids[index], id) == 0)
      return true;
  }
  return false;
}

static bool history_audio_mime_valid(const char *mime_type) {
  return mime_type != NULL &&
         strcmp(mime_type, "audio/ogg; codecs=opus") == 0;
}

static int capture_history(h2_gizclaw_client_t *client,
                           const char *workspace_name,
                           history_snapshot_t *snapshot) {
  h2_gizclaw_workspace_history_page_t page = {0};
  int rc = h2_gizclaw_client_workspace_history_list(
      client, h2_gizclaw_e2e_str(workspace_name), (h2_gizclaw_str_t){0},
      H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS,
      H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC, &page);
  if (rc == H2_PAL_OK) {
    for (size_t index = 0u; index < page.count; ++index) {
      if (page.items[index].id == NULL)
        continue;
      const size_t len = strlen(page.items[index].id);
      if (len > H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES ||
          snapshot->count >= H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS) {
        rc = H2_PAL_ERR_FORMAT;
        break;
      }
      memcpy(snapshot->ids[snapshot->count], page.items[index].id, len + 1u);
      ++snapshot->count;
    }
  }
  h2_gizclaw_workspace_history_page_deinit(client, &page);
  return rc;
}

static int decode_opus_packet(OpusDecoder *decoder, const uint8_t *packet,
                              size_t packet_len, uint64_t *pcm_samples,
                              bool *non_silent, opus_int16 *pcm,
                              size_t pcm_capacity) {
  if (packet == NULL || packet_len == 0u || packet_len > INT32_MAX ||
      pcm == NULL || pcm_capacity < H2_GIZCLAW_E2E_REPLY_MAX_SAMPLES)
    return H2_PAL_ERR_FORMAT;
  const int count = opus_decode(decoder, packet, (opus_int32)packet_len, pcm,
                                H2_GIZCLAW_E2E_REPLY_MAX_SAMPLES, 0);
  if (count <= 0) {
    printf("H2_GIZCLAW_E2E stage=opus_decode result=FAIL opus_rc=%d "
           "packet_bytes=%zu\n",
           count, packet_len);
    return H2_PAL_ERR_FORMAT;
  }
  *pcm_samples += (uint64_t)count;
  for (int index = 0; index < count; ++index)
    *non_silent = *non_silent || pcm[index] != 0;
  return H2_PAL_OK;
}

static int write_opus(h2_gizclaw_e2e_fixture_t *fixture,
                      h2_gizclaw_conversation_t *conversation,
                      size_t *out_frames) {
  const int encoder_size = opus_encoder_get_size(1);
  if (encoder_size <= 0)
    return H2_PAL_ERR_FORMAT;
  OpusEncoder *encoder =
      h2_pal_mem_alloc(fixture->allocator, (size_t)encoder_size);
  if (encoder == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  if (opus_encoder_init(encoder, H2_GIZCLAW_E2E_PCM_RATE_HZ, 1,
                        OPUS_APPLICATION_VOIP) != OPUS_OK ||
      opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)) != OPUS_OK) {
    h2_pal_mem_free(fixture->allocator, encoder);
    return H2_PAL_ERR_FORMAT;
  }
  const size_t frame_bytes = H2_GIZCLAW_E2E_PCM_FRAME_SAMPLES * sizeof(int16_t);
  opus_int16 pcm[H2_GIZCLAW_E2E_PCM_FRAME_SAMPLES];
  uint8_t opus[H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES];
  size_t offset = 0u;
  int rc = H2_PAL_OK;
  while (offset < fixture->pcm_len) {
    const size_t remaining = fixture->pcm_len - offset;
    const size_t bytes = remaining < frame_bytes ? remaining : frame_bytes;
    memset(pcm, 0, sizeof(pcm));
    memcpy(pcm, fixture->pcm + offset, bytes);
    const int opus_len =
        opus_encode(encoder, pcm, H2_GIZCLAW_E2E_PCM_FRAME_SAMPLES, opus,
                    (opus_int32)sizeof(opus));
    if (opus_len <= 0) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    do {
      rc = h2_gizclaw_conversation_write_opus(conversation, opus,
                                              (size_t)opus_len, 0u);
      if (rc == H2_PAL_ERR_WOULD_BLOCK) {
        if (!h2_gizclaw_e2e_fixture_has_time(fixture, 1000u)) {
          h2_gizclaw_e2e_evidence("h2_gizclaw_conversation_write_opus",
                                  "voice-uplink", H2_PAL_ERR_TIMEOUT);
          rc = H2_PAL_ERR_TIMEOUT;
          break;
        }
        (void)h2_pal_time_sleep_ms(fixture->time, 10u);
      }
    } while (rc == H2_PAL_ERR_WOULD_BLOCK);
    if (rc != H2_PAL_OK) {
      h2_gizclaw_e2e_evidence("h2_gizclaw_conversation_write_opus",
                              "voice-uplink", rc);
      break;
    }
    offset += bytes;
    ++*out_frames;
  }
  memset(encoder, 0, (size_t)encoder_size);
  h2_pal_mem_free(fixture->allocator, encoder);
  h2_gizclaw_e2e_evidence("h2_gizclaw_conversation_write_opus", "voice-uplink",
                          rc);
  if (rc == H2_PAL_OK) {
    do {
      rc = h2_gizclaw_conversation_commit(conversation, 0u);
      if (rc == H2_PAL_ERR_WOULD_BLOCK)
        (void)h2_pal_time_sleep_ms(fixture->time, 10u);
    } while (rc == H2_PAL_ERR_WOULD_BLOCK &&
             h2_gizclaw_e2e_fixture_has_time(fixture, 1000u));
  }
  h2_gizclaw_e2e_evidence("h2_gizclaw_conversation_commit", "voice-uplink", rc);
  return rc;
}

static int collect_reply(h2_gizclaw_e2e_fixture_t *fixture,
                         h2_gizclaw_conversation_t *conversation,
                         uint64_t generation, size_t *out_packets,
                         uint64_t *out_pcm_samples) {
  int opus_error = OPUS_OK;
  OpusDecoder *decoder =
      opus_decoder_create(H2_GIZCLAW_E2E_REPLY_RATE_HZ, 1, &opus_error);
  if (decoder == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  if (opus_error != OPUS_OK) {
    opus_decoder_destroy(decoder);
    return H2_PAL_ERR_FORMAT;
  }
  opus_int16 *pcm =
      h2_pal_mem_alloc(fixture->allocator,
                       H2_GIZCLAW_E2E_REPLY_MAX_SAMPLES * sizeof(opus_int16));
  if (pcm == NULL) {
    opus_decoder_destroy(decoder);
    return H2_PAL_ERR_NO_MEMORY;
  }
  bool text_seen = false;
  bool done_seen = false;
  bool non_silent = false;
  uint64_t started_ms = 0u;
  int rc = h2_pal_time_get_monotonic_ms(fixture->time, &started_ms);
  while (rc == H2_PAL_OK && !done_seen) {
    uint64_t now_ms = 0u;
    rc = h2_pal_time_get_monotonic_ms(fixture->time, &now_ms);
    if (rc != H2_PAL_OK)
      break;
    if (h2_pal_time_elapsed_ms(started_ms, now_ms) >=
        H2_GIZCLAW_E2E_RESPONSE_TIMEOUT_MS) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    h2_gizclaw_conversation_event_t event = {0};
    rc = h2_gizclaw_conversation_poll(conversation, 250, &event);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      rc = H2_PAL_OK;
    for (h2_gizclaw_e2e_actor_role_t role = H2_GIZCLAW_E2E_FRIEND;
         rc == H2_PAL_OK && role <= H2_GIZCLAW_E2E_GROUP_MEMBER; ++role) {
      if (fixture->actors[role].client != NULL)
        rc = h2_gizclaw_e2e_fixture_poll(fixture, role, 0u);
    }
    if (rc == H2_PAL_OK && event.kind == H2_GIZCLAW_CONVERSATION_EVENT_NONE)
      continue;
    if (rc != H2_PAL_OK)
      break;
    if (event.kind != H2_GIZCLAW_CONVERSATION_EVENT_NONE &&
        event.generation != generation) {
      rc = H2_PAL_ERR_INVALID_STATE;
      break;
    }
    if ((event.kind == H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA ||
         event.kind == H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE) &&
        event.text != NULL && event.text_len > 0u) {
      text_seen = true;
    } else if (event.kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO) {
      rc = decode_opus_packet(decoder, event.audio, event.audio_len,
                              out_pcm_samples, &non_silent, pcm,
                              H2_GIZCLAW_E2E_REPLY_MAX_SAMPLES);
      if (rc != H2_PAL_OK)
        break;
      ++*out_packets;
    } else if (event.kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE) {
      done_seen = true;
    } else if (event.kind == H2_GIZCLAW_CONVERSATION_EVENT_ERROR) {
      printf("H2_GIZCLAW_E2E stage=voice terminal=error code=%s "
             "retryable=%s\n",
             event.error_code == NULL ? "" : event.error_code,
             event.retryable ? "true" : "false");
      rc = H2_PAL_ERR_IO;
      break;
    }
  }
  opus_decoder_destroy(decoder);
  memset(pcm, 0, H2_GIZCLAW_E2E_REPLY_MAX_SAMPLES * sizeof(opus_int16));
  h2_pal_mem_free(fixture->allocator, pcm);
  if (rc == H2_PAL_OK && (!text_seen || !done_seen || *out_packets == 0u ||
                          *out_pcm_samples == 0u || !non_silent)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_e2e_evidence("h2_gizclaw_conversation_poll", "voice-downlink", rc);
  return rc;
}

static int append_audio(void *user, const uint8_t *data, size_t len) {
  byte_buffer_t *buffer = user;
  if (buffer == NULL || (data == NULL && len != 0u) ||
      len > H2_GIZCLAW_E2E_HISTORY_MAX_BYTES - buffer->len) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  const size_t required = buffer->len + len;
  if (required > buffer->capacity) {
    size_t capacity = buffer->capacity == 0u ? 4096u : buffer->capacity;
    while (capacity < required &&
           capacity <= H2_GIZCLAW_E2E_HISTORY_MAX_BYTES / 2u) {
      capacity *= 2u;
    }
    if (capacity < required)
      capacity = required;
    uint8_t *grown =
        h2_pal_mem_realloc(buffer->allocator, buffer->data, capacity);
    if (grown == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    buffer->data = grown;
    buffer->capacity = capacity;
  }
  memcpy(buffer->data + buffer->len, data, len);
  buffer->len += len;
  return H2_PAL_OK;
}

static int ogg_load_page(ogg_cursor_t *cursor) {
  if (cursor->page_offset + 27u > cursor->size)
    return 0;
  const uint8_t *page = cursor->data + cursor->page_offset;
  if (memcmp(page, "OggS", 4u) != 0)
    return -1;
  const size_t segment_count = page[26u];
  const size_t header_len = 27u + segment_count;
  if (cursor->page_offset + header_len > cursor->size)
    return -1;
  size_t payload_len = 0u;
  for (size_t index = 0u; index < segment_count; ++index)
    payload_len += page[27u + index];
  if (cursor->page_offset + header_len + payload_len > cursor->size)
    return -1;
  cursor->segments = page + 27u;
  cursor->segment_count = segment_count;
  cursor->segment_index = 0u;
  cursor->payload = page + header_len;
  cursor->payload_left = payload_len;
  cursor->page_offset += header_len + payload_len;
  return 1;
}

static int ogg_next_packet(ogg_cursor_t *cursor, const uint8_t **out_packet,
                           size_t *out_len) {
  cursor->packet_len = 0u;
  for (;;) {
    if (cursor->segment_index >= cursor->segment_count) {
      const int page_rc = ogg_load_page(cursor);
      if (page_rc <= 0)
        return page_rc;
    }
    while (cursor->segment_index < cursor->segment_count) {
      const size_t segment_len = cursor->segments[cursor->segment_index++];
      if (segment_len > cursor->payload_left ||
          segment_len > cursor->packet_capacity - cursor->packet_len) {
        return -1;
      }
      memcpy(cursor->packet + cursor->packet_len, cursor->payload, segment_len);
      cursor->packet_len += segment_len;
      cursor->payload += segment_len;
      cursor->payload_left -= segment_len;
      if (segment_len < 255u) {
        *out_packet = cursor->packet;
        *out_len = cursor->packet_len;
        return 1;
      }
    }
  }
}

static int decode_history_audio(const h2_pal_mem_api_t *allocator,
                                const uint8_t *data, size_t len,
                                uint64_t *out_samples) {
  if (allocator == NULL || data == NULL || len < 4u ||
      memcmp(data, "OggS", 4u) != 0)
    return H2_PAL_ERR_FORMAT;
  int opus_error = OPUS_OK;
  OpusDecoder *decoder =
      opus_decoder_create(H2_GIZCLAW_E2E_REPLY_RATE_HZ, 1, &opus_error);
  if (decoder == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  if (opus_error != OPUS_OK) {
    opus_decoder_destroy(decoder);
    return H2_PAL_ERR_FORMAT;
  }
  uint8_t *packet = h2_pal_mem_alloc(allocator, 65536u);
  opus_int16 *pcm = h2_pal_mem_alloc(
      allocator, H2_GIZCLAW_E2E_REPLY_MAX_SAMPLES * sizeof(opus_int16));
  if (packet == NULL || pcm == NULL) {
    h2_pal_mem_free(allocator, packet);
    h2_pal_mem_free(allocator, pcm);
    opus_decoder_destroy(decoder);
    return H2_PAL_ERR_NO_MEMORY;
  }
  ogg_cursor_t cursor = {
      .data = data,
      .size = len,
      .packet = packet,
      .packet_capacity = 65536u,
  };
  bool non_silent = false;
  int rc = H2_PAL_OK;
  for (;;) {
    const uint8_t *opus_packet = NULL;
    size_t packet_len = 0u;
    const int packet_rc = ogg_next_packet(&cursor, &opus_packet, &packet_len);
    if (packet_rc == 0)
      break;
    if (packet_rc < 0) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    if (packet_len >= 8u && (memcmp(opus_packet, "OpusHead", 8u) == 0 ||
                             memcmp(opus_packet, "OpusTags", 8u) == 0)) {
      continue;
    }
    rc = decode_opus_packet(decoder, opus_packet, packet_len, out_samples,
                            &non_silent, pcm, H2_GIZCLAW_E2E_REPLY_MAX_SAMPLES);
    if (rc != H2_PAL_OK)
      break;
  }
  opus_decoder_destroy(decoder);
  memset(packet, 0, 65536u);
  memset(pcm, 0, H2_GIZCLAW_E2E_REPLY_MAX_SAMPLES * sizeof(opus_int16));
  h2_pal_mem_free(allocator, packet);
  h2_pal_mem_free(allocator, pcm);
  if (rc == H2_PAL_OK && (*out_samples == 0u || !non_silent))
    rc = H2_PAL_ERR_INVALID_STATE;
  return rc;
}

static int verify_history(h2_gizclaw_e2e_fixture_t *fixture,
                          const history_snapshot_t *before, size_t *out_bytes,
                          uint64_t *out_samples, char *out_history_id,
                          size_t out_history_id_capacity) {
  h2_gizclaw_client_t *client = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  uint64_t started_ms = 0u;
  int rc = h2_pal_time_get_monotonic_ms(fixture->time, &started_ms);
  char history_id[H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES + 1u] = {0};
  size_t new_entries = 0u;
  size_t agent_entries = 0u;
  size_t transcript_entries = 0u;
  size_t replay_entries = 0u;
  while (rc == H2_PAL_OK && history_id[0] == '\0') {
    new_entries = 0u;
    agent_entries = 0u;
    transcript_entries = 0u;
    replay_entries = 0u;
    h2_gizclaw_workspace_history_page_t page = {0};
    rc = h2_gizclaw_client_workspace_history_list(
        client, h2_gizclaw_e2e_str(fixture->workspace_name),
        (h2_gizclaw_str_t){0}, H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS,
        H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC, &page);
    if (rc == H2_PAL_OK) {
      for (size_t index = 0u; index < page.count; ++index) {
        const h2_gizclaw_workspace_history_entry_t *entry = &page.items[index];
        if (entry->id == NULL || snapshot_contains(before, entry->id))
          continue;
        ++new_entries;
        if (entry->type != H2_GIZCLAW_WORKSPACE_HISTORY_AGENT)
          continue;
        ++agent_entries;
        if (entry->text == NULL || entry->text[0] == '\0')
          continue;
        ++transcript_entries;
        if (!entry->replay_available)
          continue;
        ++replay_entries;
        const size_t len = strlen(entry->id);
        if (len <= H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES)
          memcpy(history_id, entry->id, len + 1u);
        break;
      }
    }
    h2_gizclaw_workspace_history_page_deinit(client, &page);
    if (rc != H2_PAL_OK || history_id[0] != '\0')
      break;
    uint64_t now_ms = 0u;
    rc = h2_pal_time_get_monotonic_ms(fixture->time, &now_ms);
    if (rc == H2_PAL_OK && h2_pal_time_elapsed_ms(started_ms, now_ms) >=
                               H2_GIZCLAW_E2E_HISTORY_TIMEOUT_MS) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    (void)h2_pal_time_sleep_ms(fixture->time, 500u);
  }
  if (rc != H2_PAL_OK) {
    printf("H2_GIZCLAW_E2E stage=history_scan result=FAIL "
           "new=%zu agent=%zu transcript=%zu replay=%zu\n",
           new_entries, agent_entries, transcript_entries, replay_entries);
    return rc;
  }
  byte_buffer_t audio = {.allocator = fixture->allocator};
  h2_gizclaw_workspace_history_audio_info_t info = {0};
  rc = h2_gizclaw_client_workspace_history_audio_get(
      client, h2_gizclaw_e2e_str(fixture->workspace_name),
      h2_gizclaw_e2e_str(history_id), append_audio, &audio, &info);
  h2_gizclaw_e2e_evidence("h2_gizclaw_client_workspace_history_audio_get",
                          "history", rc);
  if (rc == H2_PAL_OK &&
      (audio.len == 0u || info.received_bytes != audio.len ||
       info.size_bytes != audio.len ||
       !history_audio_mime_valid(info.mime_type))) {
    printf("H2_GIZCLAW_E2E stage=history_metadata result=FAIL bytes=%zu "
           "received=%" PRIu64 " expected=%" PRIu64 " mime=%s\n",
           audio.len, info.received_bytes, info.size_bytes,
           info.mime_type == NULL ? "-" : info.mime_type);
    rc = H2_PAL_ERR_FORMAT;
  }
  if (rc == H2_PAL_OK)
    rc = decode_history_audio(fixture->allocator, audio.data, audio.len,
                              out_samples);
  if (rc == H2_PAL_OK) {
    const size_t history_id_len = strlen(history_id);
    if (out_history_id == NULL || history_id_len >= out_history_id_capacity) {
      rc = H2_PAL_ERR_NO_MEMORY;
    } else {
      memcpy(out_history_id, history_id, history_id_len + 1u);
    }
  }
  *out_bytes = audio.len;
  h2_gizclaw_workspace_history_audio_info_deinit(client, &info);
  h2_pal_mem_free(fixture->allocator, audio.data);
  return rc;
}

static int verify_reconnect_history(h2_gizclaw_e2e_fixture_t *fixture,
                                    const char *history_id) {
  int rc =
      h2_gizclaw_e2e_fixture_reconnect_actor(fixture, H2_GIZCLAW_E2E_OWNER);
  h2_gizclaw_client_t *client = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  h2_gizclaw_workspace_t workspace = {0};
  char *profile_name = NULL;
  char *profile_revision = NULL;
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_workspace_get(
        client, h2_gizclaw_e2e_str(fixture->workspace_name), &workspace,
        &profile_name, &profile_revision);
  }
  if (rc == H2_PAL_OK &&
      (workspace.name == NULL ||
       strcmp(workspace.name, fixture->workspace_name) != 0)) {
    rc = H2_PAL_ERR_INVALID_STATE;
  }
  h2_gizclaw_workspace_deinit(client, &workspace);
  h2_pal_mem_free(fixture->allocator, profile_name);
  h2_pal_mem_free(fixture->allocator, profile_revision);
  history_snapshot_t *after =
      h2_pal_mem_alloc(fixture->allocator, sizeof(*after));
  if (rc == H2_PAL_OK && after == NULL)
    rc = H2_PAL_ERR_NO_MEMORY;
  if (after != NULL)
    memset(after, 0, sizeof(*after));
  if (rc == H2_PAL_OK)
    rc = capture_history(client, fixture->workspace_name, after);
  if (rc == H2_PAL_OK && !snapshot_contains(after, history_id))
    rc = H2_PAL_ERR_NOT_FOUND;
  h2_pal_mem_free(fixture->allocator, after);
  printf("H2_GIZCLAW_E2E stage=workspace_history_reconnect result=%s\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL");
  return rc;
}

int h2_gizclaw_e2e_run_voice(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || fixture->pcm == NULL || fixture->pcm_len == 0u ||
      !fixture->workspace_created) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!h2_gizclaw_e2e_fixture_has_time(fixture, 130000u))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_client_t *client = fixture->actors[H2_GIZCLAW_E2E_OWNER].client;
  history_snapshot_t *before =
      h2_pal_mem_alloc(fixture->allocator, sizeof(*before));
  if (before == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(before, 0, sizeof(*before));
  int rc = capture_history(client, fixture->workspace_name, before);
  const uint64_t generation = fixture->started_ms ^ UINT64_C(0x644);
  h2_gizclaw_conversation_t *conversation = NULL;
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_conversation_open(
        client, h2_gizclaw_e2e_str(fixture->workspace_name), generation, 30000,
        &conversation);
    h2_gizclaw_e2e_evidence("h2_gizclaw_conversation_open", "voice", rc);
  }
  size_t uplink_frames = 0u;
  size_t downlink_packets = 0u;
  uint64_t downlink_samples = 0u;
  if (rc == H2_PAL_OK)
    rc = write_opus(fixture, conversation, &uplink_frames);
  if (rc == H2_PAL_OK)
    rc = collect_reply(fixture, conversation, generation, &downlink_packets,
                       &downlink_samples);
  if (rc != H2_PAL_OK)
    h2_gizclaw_conversation_cancel(conversation);
  h2_gizclaw_conversation_deinit(conversation);
  size_t history_bytes = 0u;
  uint64_t history_samples = 0u;
  char history_id[H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES + 1u] = {0};
  if (rc == H2_PAL_OK)
    rc = verify_history(fixture, before, &history_bytes, &history_samples,
                        history_id, sizeof(history_id));
  if (rc == H2_PAL_OK)
    rc = verify_reconnect_history(fixture, history_id);
  h2_pal_mem_free(fixture->allocator, before);
  printf("H2_GIZCLAW_E2E stage=voice result=%s rc=%d "
         "uplink_frames=%zu downlink_packets=%zu downlink_pcm_samples=%" PRIu64
         " history_audio_bytes=%zu history_pcm_samples=%" PRIu64
         "\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc, uplink_frames, downlink_packets,
         downlink_samples, history_bytes, history_samples);
  return rc;
}
