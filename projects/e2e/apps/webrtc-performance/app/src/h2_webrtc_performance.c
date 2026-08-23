#include "h2_webrtc_performance.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  H2_WEBRTC_PERF_MAX_ITERATIONS = 10,
  H2_WEBRTC_PERF_REQUEST_COUNT = 3,
  H2_WEBRTC_PERF_TRANSFER_SIZE = 10 * 1024 * 1024,
  H2_WEBRTC_PERF_CHUNK_SIZE = 8 * 1024,
  H2_WEBRTC_PERF_HEADER_SIZE = 16,
  H2_WEBRTC_PERF_AUDIO_FRAMES = 100,
  H2_WEBRTC_PERF_AUDIO_INTERVAL_MS = 20,
  H2_WEBRTC_PERF_AUDIO_LATE_GAP_MS = 40,
  H2_WEBRTC_PERF_AUDIO_HOST_TOLERANCE_MS = 2,
  H2_WEBRTC_PERF_AUDIO_PLAYOUT_GRACE_MS = 1000,
  H2_WEBRTC_PERF_AUDIO_SMOKE_MAX_GAP_MS = 200,
  H2_WEBRTC_PERF_AUDIO_SMOKE_MAX_MISSING = 5,
  H2_WEBRTC_PERF_EVENT_COUNT = 10,
};

enum {
  H2_WEBRTC_PERF_REQUEST = 1,
  H2_WEBRTC_PERF_UPLOAD = 2,
  H2_WEBRTC_PERF_DOWNLOAD = 3,
  H2_WEBRTC_PERF_ECHO = 4,
  H2_WEBRTC_PERF_LAST = 1,
};

static const uint8_t h2_webrtc_perf_magic[4] = {'H', '2', 'P', 'F'};
static const uint64_t h2_webrtc_perf_transfer_timeout_ns =
    UINT64_C(300000000000);

typedef struct h2_webrtc_performance_state {
  const h2_pal_webrtc_api_t *api;
  const h2_webrtc_performance_config_t *config;
  h2_runtime_t *runtime;
  h2_pal_webrtc_peer_t *peer;
  h2_pal_webrtc_peer_state_t peer_state;
  h2_pal_webrtc_channel_t *packet;
  h2_pal_webrtc_channel_t *event;
  h2_pal_webrtc_channel_t *requests[H2_WEBRTC_PERF_REQUEST_COUNT];
  unsigned request_count;
  unsigned request_expected_reply_mask;
  unsigned persistent_open_mask;
  unsigned request_open_mask;
  unsigned request_close_mask;
  unsigned request_reply_mask;
  uint64_t request_sent_ns[H2_WEBRTC_PERF_REQUEST_COUNT];
  uint64_t request_rtt_ns[H2_WEBRTC_PERF_REQUEST_COUNT];
  uint16_t request_sids[H2_WEBRTC_PERF_REQUEST_COUNT];
  uint64_t request_batch_started_ns;
  uint64_t request_batch_elapsed_ns;
  size_t download_bytes;
  size_t upload_bytes;
  uint64_t bulk_started_ns;
  uint64_t upload_started_ns;
  uint64_t upload_elapsed_ns;
  uint64_t download_elapsed_ns;
  int download_done;
  int upload_done;
  unsigned event_echoes;
  unsigned packet_echoes;
  unsigned audio_received;
  uint16_t audio_next_sequence;
  uint8_t audio_seen[(H2_WEBRTC_PERF_AUDIO_FRAMES + 7) / 8];
  unsigned audio_duplicates;
  unsigned audio_reordered;
  uint64_t audio_last_arrival_ns;
  uint64_t audio_gap_ns[H2_WEBRTC_PERF_AUDIO_FRAMES];
  uint64_t audio_max_gap_ns;
  uint16_t audio_max_gap_sequence;
  uint64_t audio_max_rtt_ns;
  unsigned audio_late_gaps;
  unsigned audio_deadline_misses;
  unsigned audio_would_block;
  uint64_t poll_calls;
  int callback_error;
  int callback_error_reason;
  char offer[16384];
  size_t offer_len;
} h2_webrtc_performance_state_t;

typedef struct h2_webrtc_performance_transfer {
  uint64_t bulk_elapsed_ns;
  uint64_t upload_elapsed_ns;
  uint64_t download_elapsed_ns;
} h2_webrtc_performance_transfer_t;

static uint64_t
h2_webrtc_perf_now_ns(const h2_webrtc_performance_state_t *state) {
  uint64_t now_us = 0u;
  if (h2_pal_time_get_monotonic_us(state->runtime->time, &now_us) !=
      H2_PAL_OK) {
    return 0u;
  }
  return now_us * 1000u;
}

static void h2_webrtc_perf_sleep_ms(const h2_webrtc_performance_state_t *state,
                                    uint32_t milliseconds) {
  (void)h2_pal_time_sleep_ms(state->runtime->time, milliseconds);
}

static void
h2_webrtc_perf_checkpoint(const h2_webrtc_performance_state_t *state,
                          const char *name) {
  if (state->config->checkpoint != NULL) {
    state->config->checkpoint(state->config->checkpoint_user, name);
  }
}

static void h2_webrtc_perf_write_u16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t)(value >> 8u);
  out[1] = (uint8_t)value;
}

static void h2_webrtc_perf_write_u32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)(value >> 24u);
  out[1] = (uint8_t)(value >> 16u);
  out[2] = (uint8_t)(value >> 8u);
  out[3] = (uint8_t)value;
}

static void h2_webrtc_perf_write_u64(uint8_t *out, uint64_t value) {
  for (unsigned index = 0u; index < 8u; ++index) {
    out[index] = (uint8_t)(value >> (56u - index * 8u));
  }
}

static uint16_t h2_webrtc_perf_read_u16(const uint8_t *data) {
  return (uint16_t)((uint16_t)data[0] << 8u | data[1]);
}

static uint32_t h2_webrtc_perf_read_u32(const uint8_t *data) {
  return (uint32_t)data[0] << 24u | (uint32_t)data[1] << 16u |
         (uint32_t)data[2] << 8u | data[3];
}

static uint64_t h2_webrtc_perf_read_u64(const uint8_t *data) {
  uint64_t value = 0u;
  for (unsigned index = 0u; index < 8u; ++index) {
    value = value << 8u | data[index];
  }
  return value;
}

static void h2_webrtc_perf_header(uint8_t *out, uint8_t op, uint8_t flags,
                                  uint16_t sequence, uint32_t total,
                                  uint32_t offset) {
  memset(out, 0, H2_WEBRTC_PERF_HEADER_SIZE);
  memcpy(out, h2_webrtc_perf_magic, sizeof(h2_webrtc_perf_magic));
  out[4] = op;
  out[5] = flags;
  h2_webrtc_perf_write_u16(out + 6u, sequence);
  h2_webrtc_perf_write_u32(out + 8u, total);
  h2_webrtc_perf_write_u32(out + 12u, offset);
}

static int
h2_webrtc_perf_request_index(const h2_webrtc_performance_state_t *state,
                             h2_pal_webrtc_channel_t *channel) {
  for (int index = 0; index < H2_WEBRTC_PERF_REQUEST_COUNT; ++index) {
    if (state->requests[index] == channel) {
      return index;
    }
  }
  return -1;
}

static void h2_webrtc_perf_on_peer_state(void *user, h2_pal_webrtc_peer_t *peer,
                                         h2_pal_webrtc_peer_state_t state) {
  (void)peer;
  ((h2_webrtc_performance_state_t *)user)->peer_state = state;
}

static void h2_webrtc_perf_on_local_sdp(void *user, h2_pal_webrtc_peer_t *peer,
                                        h2_pal_webrtc_sdp_type_t type,
                                        h2_pal_webrtc_str_t sdp) {
  (void)peer;
  h2_webrtc_performance_state_t *state = user;
  if (type != H2_PAL_WEBRTC_SDP_OFFER || state->offer_len != 0u ||
      sdp.data == NULL || sdp.len == 0u || sdp.len >= sizeof(state->offer)) {
    state->callback_error = 1;
    state->callback_error_reason = 1;
    return;
  }
  memcpy(state->offer, sdp.data, sdp.len);
  state->offer[sdp.len] = '\0';
  state->offer_len = sdp.len;
}

static void
h2_webrtc_perf_on_channel_state(void *user, h2_pal_webrtc_peer_t *peer,
                                h2_pal_webrtc_channel_t *channel,
                                const h2_pal_webrtc_channel_info_t *info,
                                h2_pal_webrtc_channel_state_t channel_state) {
  (void)peer;
  h2_webrtc_performance_state_t *state = user;
  if (info == NULL || !info->has_stream_id) {
    state->callback_error = 1;
    state->callback_error_reason = 2;
    return;
  }
  if (channel == state->packet) {
    if (info->ordered || info->reliable) {
      state->callback_error = 1;
      state->callback_error_reason = 3;
    } else if (channel_state == H2_PAL_WEBRTC_CHANNEL_OPEN) {
      state->persistent_open_mask |= 1u;
    } else if (channel_state == H2_PAL_WEBRTC_CHANNEL_CLOSED ||
               channel_state == H2_PAL_WEBRTC_CHANNEL_ERROR) {
      state->packet = NULL;
      state->callback_error = 1;
      state->callback_error_reason = 4;
    }
    return;
  }
  if (channel == state->event) {
    if (!info->ordered || !info->reliable) {
      state->callback_error = 1;
      state->callback_error_reason = 5;
    } else if (channel_state == H2_PAL_WEBRTC_CHANNEL_OPEN) {
      state->persistent_open_mask |= 2u;
    } else if (channel_state == H2_PAL_WEBRTC_CHANNEL_CLOSED ||
               channel_state == H2_PAL_WEBRTC_CHANNEL_ERROR) {
      state->event = NULL;
      state->callback_error = 1;
      state->callback_error_reason = 6;
    }
    return;
  }
  const int index = h2_webrtc_perf_request_index(state, channel);
  if (index < 0 || !info->ordered || !info->reliable) {
    state->callback_error = 1;
    state->callback_error_reason = 7;
    return;
  }
  if (channel_state == H2_PAL_WEBRTC_CHANNEL_OPEN) {
    for (int other = 0; other < H2_WEBRTC_PERF_REQUEST_COUNT; ++other) {
      if (other != index && state->request_sids[other] == info->stream_id) {
        state->callback_error = 1;
        return;
      }
    }
    state->request_sids[index] = info->stream_id;
    state->request_open_mask |= 1u << index;
  } else if (channel_state == H2_PAL_WEBRTC_CHANNEL_CLOSED) {
    state->request_close_mask |= 1u << index;
    state->requests[index] = NULL;
  } else if (channel_state == H2_PAL_WEBRTC_CHANNEL_ERROR) {
    state->callback_error = 1;
  }
}

static void h2_webrtc_perf_on_channel_message(
    void *user, h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_channel_t *channel,
    const h2_pal_webrtc_channel_info_t *info, const uint8_t *data, size_t len,
    int is_text) {
  (void)peer;
  (void)info;
  h2_webrtc_performance_state_t *state = user;
  if (is_text || data == NULL || len < H2_WEBRTC_PERF_HEADER_SIZE ||
      memcmp(data, h2_webrtc_perf_magic, sizeof(h2_webrtc_perf_magic)) != 0) {
    state->callback_error = 1;
    return;
  }
  const uint8_t op = data[4];
  const uint8_t flags = data[5];
  const uint16_t sequence = h2_webrtc_perf_read_u16(data + 6u);
  const uint32_t total = h2_webrtc_perf_read_u32(data + 8u);
  const uint32_t offset = h2_webrtc_perf_read_u32(data + 12u);
  const int request_index = h2_webrtc_perf_request_index(state, channel);
  if (op == H2_WEBRTC_PERF_REQUEST && request_index >= 0 &&
      sequence < H2_WEBRTC_PERF_REQUEST_COUNT &&
      sequence == (uint16_t)request_index) {
    const uint64_t now_ns = h2_webrtc_perf_now_ns(state);
    state->request_rtt_ns[request_index] =
        now_ns - state->request_sent_ns[request_index];
    state->request_reply_mask |= 1u << request_index;
    if (state->request_reply_mask == state->request_expected_reply_mask) {
      state->request_batch_elapsed_ns =
          now_ns - state->request_batch_started_ns;
    }
    return;
  }
  if (channel == state->requests[0] && op == H2_WEBRTC_PERF_UPLOAD &&
      flags == H2_WEBRTC_PERF_LAST && total == H2_WEBRTC_PERF_TRANSFER_SIZE &&
      offset == total && len == H2_WEBRTC_PERF_HEADER_SIZE) {
    state->upload_done = 1;
    state->upload_elapsed_ns =
        h2_webrtc_perf_now_ns(state) - state->upload_started_ns;
    return;
  }
  if (channel == state->requests[0] && op == H2_WEBRTC_PERF_DOWNLOAD &&
      total == H2_WEBRTC_PERF_TRANSFER_SIZE &&
      offset == state->download_bytes && len > H2_WEBRTC_PERF_HEADER_SIZE &&
      len - H2_WEBRTC_PERF_HEADER_SIZE <= H2_WEBRTC_PERF_CHUNK_SIZE) {
    for (size_t index = H2_WEBRTC_PERF_HEADER_SIZE; index < len; ++index) {
      if (data[index] !=
          (uint8_t)(offset + index - H2_WEBRTC_PERF_HEADER_SIZE)) {
        state->callback_error = 1;
        return;
      }
    }
    state->download_bytes += len - H2_WEBRTC_PERF_HEADER_SIZE;
    if (state->download_bytes == H2_WEBRTC_PERF_TRANSFER_SIZE ||
        (state->download_bytes % (1024u * 1024u)) <
            len - H2_WEBRTC_PERF_HEADER_SIZE) {
      const uint64_t elapsed_ns =
          h2_webrtc_perf_now_ns(state) - state->bulk_started_ns;
      const uint64_t bytes_per_second =
          elapsed_ns == 0u ? 0u
                           : (uint64_t)state->download_bytes *
                                 UINT64_C(1000000000) / elapsed_ns;
      printf("H2_WEBRTC_PERF download_progress bytes=%zu total=%u "
             "elapsed_ms=%" PRIu64 " Bps=%" PRIu64 "\n",
             state->download_bytes, H2_WEBRTC_PERF_TRANSFER_SIZE,
             elapsed_ns / UINT64_C(1000000), bytes_per_second);
      h2_webrtc_perf_checkpoint(state, "download_progress");
    }
    if ((flags & H2_WEBRTC_PERF_LAST) != 0u) {
      state->download_done =
          state->download_bytes == H2_WEBRTC_PERF_TRANSFER_SIZE;
      state->download_elapsed_ns =
          h2_webrtc_perf_now_ns(state) - state->bulk_started_ns;
    }
    return;
  }
  if (op == H2_WEBRTC_PERF_ECHO &&
      (channel == state->event || channel == state->packet)) {
    if (channel == state->event) {
      state->event_echoes++;
    } else {
      state->packet_echoes++;
    }
    return;
  }
  state->callback_error = 1;
}

static void h2_webrtc_perf_on_opus(void *user, h2_pal_webrtc_peer_t *peer,
                                   const uint8_t *opus, size_t opus_len) {
  (void)peer;
  h2_webrtc_performance_state_t *state = user;
  if (opus == NULL || opus_len != 10u) {
    state->callback_error = 1;
    return;
  }
  const uint16_t sequence = h2_webrtc_perf_read_u16(opus);
  if (sequence >= H2_WEBRTC_PERF_AUDIO_FRAMES) {
    state->callback_error = 1;
    return;
  }
  const uint8_t sequence_mask = (uint8_t)(1u << (sequence & 7u));
  uint8_t *const seen = &state->audio_seen[sequence >> 3u];
  if ((*seen & sequence_mask) != 0u) {
    state->audio_duplicates++;
    return;
  }
  *seen |= sequence_mask;
  if (sequence < state->audio_next_sequence) {
    state->audio_reordered++;
  }
  if (sequence > state->audio_next_sequence) {
    printf("H2_WEBRTC_PERF audio_sequence_gap expected=%u actual=%u "
           "received=%u\n",
           state->audio_next_sequence, sequence, state->audio_received);
  }
  const uint64_t now_ns = h2_webrtc_perf_now_ns(state);
  const uint64_t sent_ns = h2_webrtc_perf_read_u64(opus + 2u);
  if (sent_ns > now_ns) {
    state->callback_error = 1;
    return;
  }
  const uint64_t rtt_ns = now_ns - sent_ns;
  if (rtt_ns > state->audio_max_rtt_ns) {
    state->audio_max_rtt_ns = rtt_ns;
  }
  const uint64_t previous_ns = state->audio_last_arrival_ns == 0u
                                   ? state->bulk_started_ns
                                   : state->audio_last_arrival_ns;
  const uint64_t gap_ns = now_ns - previous_ns;
  state->audio_gap_ns[state->audio_received] = gap_ns;
  if (gap_ns > state->audio_max_gap_ns) {
    state->audio_max_gap_ns = gap_ns;
    state->audio_max_gap_sequence = sequence;
  }
  if (gap_ns > (uint64_t)(H2_WEBRTC_PERF_AUDIO_LATE_GAP_MS +
                          H2_WEBRTC_PERF_AUDIO_HOST_TOLERANCE_MS) *
                   1000000u) {
    state->audio_late_gaps++;
  }
  state->audio_last_arrival_ns = now_ns;
  if (sequence >= state->audio_next_sequence) {
    state->audio_next_sequence = sequence + 1u;
  }
  state->audio_received++;
}

static int h2_webrtc_perf_drain_channel(h2_webrtc_performance_state_t *state,
                                        h2_pal_webrtc_channel_t *channel) {
  if (channel == NULL) {
    return 0;
  }
  uint8_t data[H2_WEBRTC_PERF_HEADER_SIZE + H2_WEBRTC_PERF_CHUNK_SIZE];
  for (size_t slot = 0u; slot < 4u; ++slot) {
    size_t len = 0u;
    int is_text = 0;
    const h2_pal_result_t result = h2_pal_webrtc_channel_receive(
        state->api, channel, data, sizeof(data), &len, &is_text, 0u);
    if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT) {
      return 0;
    }
    if (result != H2_PAL_OK) {
      return -1;
    }
    h2_webrtc_perf_on_channel_message(state, state->peer, channel, NULL, data,
                                      len, is_text);
    if (state->callback_error) {
      return -1;
    }
  }
  return 0;
}

static int h2_webrtc_perf_drain_received(h2_webrtc_performance_state_t *state) {
  uint8_t opus[256];
  for (size_t slot = 0u; slot < 4u; ++slot) {
    size_t opus_len = 0u;
    const h2_pal_result_t opus_result = h2_pal_webrtc_peer_receive_opus(
        state->api, state->peer, opus, sizeof(opus), &opus_len, 0u);
    if (opus_result == H2_PAL_OK) {
      h2_webrtc_perf_on_opus(state, state->peer, opus, opus_len);
    } else if (opus_result == H2_PAL_ERR_WOULD_BLOCK ||
               opus_result == H2_PAL_ERR_TIMEOUT) {
      break;
    } else {
      return -1;
    }
  }
  if (h2_webrtc_perf_drain_channel(state, state->packet) != 0 ||
      h2_webrtc_perf_drain_channel(state, state->event) != 0) {
    return -1;
  }
  for (size_t index = 0u; index < state->request_count; ++index) {
    if (h2_webrtc_perf_drain_channel(state, state->requests[index]) != 0) {
      return -1;
    }
  }
  return state->callback_error ? -1 : 0;
}

static int h2_webrtc_perf_poll(h2_webrtc_performance_state_t *state) {
  state->poll_calls++;
  const h2_pal_result_t result =
      h2_pal_webrtc_peer_poll(state->api, state->peer, 2u);
  if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK &&
      result != H2_PAL_ERR_TIMEOUT) {
    printf("H2_WEBRTC_PERF stage=peer_poll status=ERROR rc=%d\n", result);
    return -1;
  }
  if (state->callback_error) {
    return -1;
  }
  return h2_webrtc_perf_drain_received(state);
}

static int
h2_webrtc_perf_wait(h2_webrtc_performance_state_t *state,
                    int (*done)(const h2_webrtc_performance_state_t *),
                    uint32_t timeout_ms) {
  const uint64_t deadline =
      h2_webrtc_perf_now_ns(state) + (uint64_t)timeout_ms * 1000000u;
  while (!done(state) && !state->callback_error &&
         h2_webrtc_perf_now_ns(state) < deadline) {
    if (h2_webrtc_perf_poll(state) != 0) {
      return -1;
    }
    h2_webrtc_perf_sleep_ms(state, 1u);
  }
  return done(state) && !state->callback_error ? 0 : -1;
}

static int
h2_webrtc_perf_connected(const h2_webrtc_performance_state_t *state) {
  return state->peer_state == H2_PAL_WEBRTC_PEER_CONNECTED &&
         state->persistent_open_mask == 3u;
}

static int
h2_webrtc_perf_offer_ready(const h2_webrtc_performance_state_t *state) {
  return state->offer_len != 0u;
}

static int
h2_webrtc_perf_requests_open(const h2_webrtc_performance_state_t *state) {
  return state->request_open_mask == (1u << state->request_count) - 1u;
}

static int
h2_webrtc_perf_requests_done(const h2_webrtc_performance_state_t *state) {
  return state->request_reply_mask == state->request_expected_reply_mask;
}

static int
h2_webrtc_perf_requests_closed(const h2_webrtc_performance_state_t *state) {
  return state->request_close_mask == (1u << state->request_count) - 1u;
}

static h2_pal_result_t h2_webrtc_perf_send(h2_webrtc_performance_state_t *state,
                                           h2_pal_webrtc_channel_t *channel,
                                           const uint8_t *data, size_t len) {
  const uint64_t deadline = h2_webrtc_perf_now_ns(state) + 10000000000u;
  do {
    const h2_pal_result_t result =
        h2_pal_webrtc_channel_send(state->api, channel, data, len, 0);
    if (result != H2_PAL_ERR_WOULD_BLOCK) {
      return result;
    }
    if (h2_webrtc_perf_poll(state) != 0) {
      return H2_PAL_ERR_IO;
    }
  } while (h2_webrtc_perf_now_ns(state) < deadline);
  return H2_PAL_ERR_TIMEOUT;
}

static int h2_webrtc_perf_create_requests(h2_webrtc_performance_state_t *state,
                                          unsigned request_count) {
  if (request_count == 0u || request_count > H2_WEBRTC_PERF_REQUEST_COUNT) {
    return -1;
  }
  state->request_count = request_count;
  state->request_open_mask = 0u;
  state->request_close_mask = 0u;
  state->request_reply_mask = 0u;
  memset(state->request_rtt_ns, 0, sizeof(state->request_rtt_ns));
  memset(state->request_sids, 0, sizeof(state->request_sids));
  const char *label = "giznet/v1/service/0";
  const h2_pal_webrtc_channel_config_t config = {
      .label = {.data = label, .len = strlen(label)},
      .ordered = 1,
      .reliable = 1,
  };
  for (size_t index = 0u; index < state->request_count; ++index) {
    if (h2_pal_webrtc_peer_create_data_channel(state->api, state->peer, &config,
                                               &state->requests[index]) !=
        H2_PAL_OK) {
      return -1;
    }
  }
  return h2_webrtc_perf_wait(state, h2_webrtc_perf_requests_open, 10000u);
}

static int h2_webrtc_perf_send_requests(h2_webrtc_performance_state_t *state,
                                        unsigned first_request) {
  if (first_request >= state->request_count) {
    return -1;
  }
  state->request_reply_mask = 0u;
  state->request_expected_reply_mask =
      ((1u << state->request_count) - 1u) & ~((1u << first_request) - 1u);
  state->request_batch_elapsed_ns = 0u;
  state->request_batch_started_ns = h2_webrtc_perf_now_ns(state);
  for (size_t index = first_request; index < state->request_count; ++index) {
    uint8_t request[H2_WEBRTC_PERF_HEADER_SIZE];
    h2_webrtc_perf_header(request, H2_WEBRTC_PERF_REQUEST, 0u, (uint16_t)index,
                          0u, 0u);
    state->request_sent_ns[index] = h2_webrtc_perf_now_ns(state);
    if (h2_webrtc_perf_send(state, state->requests[index], request,
                            sizeof(request)) != H2_PAL_OK) {
      return -1;
    }
  }
  return 0;
}

static int h2_webrtc_perf_close_requests(h2_webrtc_performance_state_t *state) {
  for (size_t index = 0u; index < state->request_count; ++index) {
    h2_pal_webrtc_channel_close(state->api, state->requests[index]);
  }
  if (h2_webrtc_perf_wait(state, h2_webrtc_perf_requests_closed, 10000u) != 0) {
    return -1;
  }
  memset(state->requests, 0, sizeof(state->requests));
  state->request_count = 0u;
  state->request_expected_reply_mask = 0u;
  return 0;
}

static uint64_t
h2_webrtc_perf_request_max_rtt(const h2_webrtc_performance_state_t *state) {
  uint64_t maximum = 0u;
  for (size_t index = 0u; index < H2_WEBRTC_PERF_REQUEST_COUNT; ++index) {
    if (state->request_rtt_ns[index] > maximum) {
      maximum = state->request_rtt_ns[index];
    }
  }
  return maximum;
}

static uint64_t
h2_webrtc_perf_median(uint64_t values[H2_WEBRTC_PERF_MAX_ITERATIONS],
                      unsigned count) {
  for (size_t index = 1u; index < count; ++index) {
    const uint64_t value = values[index];
    size_t insert = index;
    while (insert > 0u && values[insert - 1u] > value) {
      values[insert] = values[insert - 1u];
      insert--;
    }
    values[insert] = value;
  }
  if (count % 2u != 0u) {
    return values[count / 2u];
  }
  const uint64_t lower = values[count / 2u - 1u];
  const uint64_t upper = values[count / 2u];
  return lower / 2u + upper / 2u + (lower % 2u + upper % 2u) / 2u;
}

static uint64_t
h2_webrtc_perf_audio_gap_p99(const h2_webrtc_performance_state_t *state) {
  uint64_t values[H2_WEBRTC_PERF_AUDIO_FRAMES];
  const size_t count = state->audio_received;
  for (size_t index = 0u; index < count; ++index) {
    values[index] = state->audio_gap_ns[index];
  }
  if (count == 0u) {
    return UINT64_MAX;
  }
  for (size_t index = 1u; index < count; ++index) {
    const uint64_t value = values[index];
    size_t insert = index;
    while (insert > 0u && values[insert - 1u] > value) {
      values[insert] = values[insert - 1u];
      insert--;
    }
    values[insert] = value;
  }
  return values[(count * 99u + 99u) / 100u - 1u];
}

static int
h2_webrtc_perf_run_transfer(h2_webrtc_performance_state_t *state, int loaded,
                            int download_only,
                            h2_webrtc_performance_transfer_t *out_transfer) {
  state->download_bytes = 0u;
  state->upload_bytes = 0u;
  state->upload_started_ns = 0u;
  state->upload_elapsed_ns = 0u;
  state->download_elapsed_ns = 0u;
  state->download_done = 0;
  state->upload_done = download_only;
  state->event_echoes = 0u;
  state->packet_echoes = 0u;
  state->audio_received = 0u;
  state->audio_next_sequence = 0u;
  memset(state->audio_seen, 0, sizeof(state->audio_seen));
  state->audio_duplicates = 0u;
  state->audio_reordered = 0u;
  state->audio_last_arrival_ns = 0u;
  memset(state->audio_gap_ns, 0, sizeof(state->audio_gap_ns));
  state->audio_max_gap_ns = 0u;
  state->audio_max_gap_sequence = 0u;
  state->audio_max_rtt_ns = 0u;
  state->audio_late_gaps = 0u;
  state->audio_deadline_misses = 0u;
  state->audio_would_block = 0u;

  uint8_t download_request[H2_WEBRTC_PERF_HEADER_SIZE];
  h2_webrtc_perf_header(download_request, H2_WEBRTC_PERF_DOWNLOAD, 0u, 0u,
                        H2_WEBRTC_PERF_TRANSFER_SIZE, 0u);
  state->bulk_started_ns = h2_webrtc_perf_now_ns(state);
  if (h2_webrtc_perf_send(state, state->requests[0], download_request,
                          sizeof(download_request)) != H2_PAL_OK ||
      (loaded && h2_webrtc_perf_send_requests(state, 1u) != 0)) {
    return -1;
  }
  if (loaded) {
    uint8_t packet[H2_WEBRTC_PERF_HEADER_SIZE];
    h2_webrtc_perf_header(packet, H2_WEBRTC_PERF_ECHO, 0u, 0u, 0u, 0u);
    if (h2_webrtc_perf_send(state, state->packet, packet, sizeof(packet)) !=
        H2_PAL_OK) {
      return -1;
    }
  }

  size_t upload_offset = 0u;
  unsigned audio_sent = 0u;
  uint64_t audio_completed_ns = 0u;
  unsigned events_sent = 0u;
  uint64_t bulk_completed_ns = 0u;
  uint64_t next_audio_ns = state->bulk_started_ns;
  const uint64_t deadline =
      state->bulk_started_ns + h2_webrtc_perf_transfer_timeout_ns;
  uint8_t upload[H2_WEBRTC_PERF_HEADER_SIZE + H2_WEBRTC_PERF_CHUNK_SIZE];
  memset(upload + H2_WEBRTC_PERF_HEADER_SIZE, 0xa5, H2_WEBRTC_PERF_CHUNK_SIZE);
  while (h2_webrtc_perf_now_ns(state) < deadline && !state->callback_error) {
    const uint64_t now_ns = h2_webrtc_perf_now_ns(state);
    if (!download_only && state->download_done &&
        state->upload_started_ns == 0u) {
      state->upload_started_ns = now_ns;
    }
    if (loaded && audio_sent < H2_WEBRTC_PERF_AUDIO_FRAMES &&
        now_ns >= next_audio_ns) {
      uint8_t opus[10];
      h2_webrtc_perf_write_u16(opus, (uint16_t)audio_sent);
      h2_webrtc_perf_write_u64(opus + 2u, now_ns);
      if (now_ns > next_audio_ns +
                       (uint64_t)H2_WEBRTC_PERF_AUDIO_INTERVAL_MS * 1000000u) {
        state->audio_deadline_misses++;
      }
      const h2_pal_result_t result = h2_pal_webrtc_peer_send_opus(
          state->api, state->peer, opus, sizeof(opus));
      if (result == H2_PAL_OK) {
        audio_sent++;
        next_audio_ns += (uint64_t)H2_WEBRTC_PERF_AUDIO_INTERVAL_MS * 1000000u;
        if (audio_sent == H2_WEBRTC_PERF_AUDIO_FRAMES) {
          audio_completed_ns = now_ns;
        }
      } else if (result != H2_PAL_ERR_WOULD_BLOCK) {
        return -1;
      } else {
        state->audio_would_block++;
      }
    }
    if (!download_only && state->upload_started_ns != 0u &&
        upload_offset < H2_WEBRTC_PERF_TRANSFER_SIZE) {
      const size_t remaining = H2_WEBRTC_PERF_TRANSFER_SIZE - upload_offset;
      const size_t payload_len = remaining < H2_WEBRTC_PERF_CHUNK_SIZE
                                     ? remaining
                                     : H2_WEBRTC_PERF_CHUNK_SIZE;
      const uint8_t flags =
          upload_offset + payload_len == H2_WEBRTC_PERF_TRANSFER_SIZE
              ? H2_WEBRTC_PERF_LAST
              : 0u;
      h2_webrtc_perf_header(
          upload, H2_WEBRTC_PERF_UPLOAD, flags,
          (uint16_t)(upload_offset / H2_WEBRTC_PERF_CHUNK_SIZE),
          H2_WEBRTC_PERF_TRANSFER_SIZE, (uint32_t)upload_offset);
      const h2_pal_result_t result = h2_pal_webrtc_channel_send(
          state->api, state->requests[0], upload,
          H2_WEBRTC_PERF_HEADER_SIZE + payload_len, 0);
      if (result == H2_PAL_OK) {
        upload_offset += payload_len;
        state->upload_bytes = upload_offset;
      } else if (result != H2_PAL_ERR_WOULD_BLOCK) {
        return -1;
      }
    }
    if (loaded && events_sent < H2_WEBRTC_PERF_EVENT_COUNT &&
        audio_sent >= (events_sent + 1u) * (H2_WEBRTC_PERF_AUDIO_FRAMES /
                                            H2_WEBRTC_PERF_EVENT_COUNT)) {
      uint8_t event[H2_WEBRTC_PERF_HEADER_SIZE];
      h2_webrtc_perf_header(event, H2_WEBRTC_PERF_ECHO, 0u,
                            (uint16_t)events_sent, 0u, 0u);
      const h2_pal_result_t result = h2_pal_webrtc_channel_send(
          state->api, state->event, event, sizeof(event), 0);
      if (result == H2_PAL_OK) {
        events_sent++;
      } else if (result != H2_PAL_ERR_WOULD_BLOCK) {
        return -1;
      }
    }
    if (h2_webrtc_perf_poll(state) != 0) {
      return -1;
    }
    if (bulk_completed_ns == 0u && state->upload_done && state->download_done) {
      bulk_completed_ns = h2_webrtc_perf_now_ns(state);
    }
    const int audio_done =
        audio_completed_ns != 0u &&
        h2_webrtc_perf_now_ns(state) >=
            audio_completed_ns +
                (uint64_t)H2_WEBRTC_PERF_AUDIO_PLAYOUT_GRACE_MS * 1000000u;
    const int auxiliary_done =
        !loaded || (h2_webrtc_perf_requests_done(state) &&
                    state->event_echoes == H2_WEBRTC_PERF_EVENT_COUNT &&
                    state->packet_echoes == 1u && audio_done);
    if (bulk_completed_ns != 0u && auxiliary_done) {
      out_transfer->bulk_elapsed_ns =
          bulk_completed_ns - state->bulk_started_ns;
      out_transfer->upload_elapsed_ns = state->upload_elapsed_ns;
      out_transfer->download_elapsed_ns = state->download_elapsed_ns;
      return 0;
    }
  }
  return -1;
}

static int h2_webrtc_perf_run_audio_only(h2_webrtc_performance_state_t *state) {
  state->audio_received = 0u;
  state->audio_next_sequence = 0u;
  memset(state->audio_seen, 0, sizeof(state->audio_seen));
  state->audio_duplicates = 0u;
  state->audio_reordered = 0u;
  state->audio_last_arrival_ns = 0u;
  memset(state->audio_gap_ns, 0, sizeof(state->audio_gap_ns));
  state->audio_max_gap_ns = 0u;
  state->audio_max_gap_sequence = 0u;
  state->audio_max_rtt_ns = 0u;
  state->audio_late_gaps = 0u;
  state->audio_deadline_misses = 0u;
  state->audio_would_block = 0u;
  state->bulk_started_ns = h2_webrtc_perf_now_ns(state);
  uint64_t next_audio_ns = state->bulk_started_ns;
  const uint64_t deadline = state->bulk_started_ns + 5000000000u;
  unsigned audio_sent = 0u;
  uint64_t audio_completed_ns = 0u;
  while (h2_webrtc_perf_now_ns(state) < deadline && !state->callback_error) {
    const uint64_t now_ns = h2_webrtc_perf_now_ns(state);
    if (audio_sent < H2_WEBRTC_PERF_AUDIO_FRAMES && now_ns >= next_audio_ns) {
      uint8_t opus[10];
      h2_webrtc_perf_write_u16(opus, (uint16_t)audio_sent);
      h2_webrtc_perf_write_u64(opus + 2u, now_ns);
      if (now_ns > next_audio_ns +
                       (uint64_t)H2_WEBRTC_PERF_AUDIO_INTERVAL_MS * 1000000u) {
        state->audio_deadline_misses++;
      }
      const h2_pal_result_t result = h2_pal_webrtc_peer_send_opus(
          state->api, state->peer, opus, sizeof(opus));
      if (result == H2_PAL_OK) {
        audio_sent++;
        next_audio_ns += (uint64_t)H2_WEBRTC_PERF_AUDIO_INTERVAL_MS * 1000000u;
        if (audio_sent == H2_WEBRTC_PERF_AUDIO_FRAMES) {
          audio_completed_ns = now_ns;
        }
      } else if (result == H2_PAL_ERR_WOULD_BLOCK) {
        state->audio_would_block++;
      } else {
        return -1;
      }
    }
    if (h2_webrtc_perf_poll(state) != 0) {
      return -1;
    }
    if (audio_completed_ns != 0u &&
        h2_webrtc_perf_now_ns(state) >=
            audio_completed_ns +
                (uint64_t)H2_WEBRTC_PERF_AUDIO_PLAYOUT_GRACE_MS * 1000000u) {
      return 0;
    }
    h2_webrtc_perf_sleep_ms(state, 1u);
  }
  return -1;
}

static int h2_webrtc_perf_connect(h2_webrtc_performance_state_t *state) {
  h2_pal_webrtc_callbacks_t callbacks = {
      .user = state,
      .on_peer_state = h2_webrtc_perf_on_peer_state,
      .on_local_sdp = h2_webrtc_perf_on_local_sdp,
      .on_channel_state = h2_webrtc_perf_on_channel_state,
      .on_channel_message = NULL,
      .on_opus_frame = NULL,
  };
  if (h2_pal_webrtc_peer_create_pull(state->api, &callbacks,
                                     H2_PAL_WEBRTC_RECEIVE_CHANNEL_PULL |
                                         H2_PAL_WEBRTC_RECEIVE_OPUS_PULL,
                                     &state->peer) != H2_PAL_OK) {
    printf("H2_WEBRTC_PERF stage=peer_create status=ERROR\n");
    return -1;
  }
  const char *labels[2] = {"giznet/v1/packet", "giznet/v1/service/32"};
  h2_pal_webrtc_channel_t **channels[2] = {&state->packet, &state->event};
  for (size_t index = 0u; index < 2u; ++index) {
    const h2_pal_webrtc_channel_config_t config = {
        .label = {.data = labels[index], .len = strlen(labels[index])},
        .ordered = index != 0u,
        .reliable = index != 0u,
    };
    const h2_pal_result_t channel_result =
        h2_pal_webrtc_peer_create_data_channel(state->api, state->peer, &config,
                                               channels[index]);
    if (channel_result != H2_PAL_OK) {
      printf("H2_WEBRTC_PERF stage=channel_create status=ERROR index=%zu "
             "rc=%d\n",
             index, channel_result);
      return -1;
    }
  }
  if (state->config->stun_url.data == NULL ||
      state->config->stun_url.len == 0u) {
    printf("H2_WEBRTC_PERF stage=stun_config status=ERROR\n");
    return -1;
  }
  const h2_pal_webrtc_ice_server_t ice_server = {
      .url = state->config->stun_url,
  };
  const h2_pal_result_t ice_result =
      h2_pal_webrtc_peer_add_ice_server(state->api, state->peer, &ice_server);
  if (ice_result != H2_PAL_OK) {
    printf("H2_WEBRTC_PERF stage=ice_server status=ERROR rc=%d\n", ice_result);
    return -1;
  }
  const h2_pal_result_t offer_result =
      h2_pal_webrtc_peer_start_offer(state->api, state->peer);
  if (offer_result != H2_PAL_OK) {
    printf("H2_WEBRTC_PERF stage=offer_start status=ERROR rc=%d\n",
           offer_result);
    return -1;
  }
  if (h2_webrtc_perf_wait(state, h2_webrtc_perf_offer_ready, 10000u) != 0) {
    printf("H2_WEBRTC_PERF stage=offer_wait status=ERROR callback_error=%d "
           "offer_len=%zu\n",
           state->callback_error, state->offer_len);
    return -1;
  }
  char answer[16384];
  size_t answer_len = 0u;
  const int exchange_result = state->config->exchange_offer(
      state->config->exchange_offer_user,
      (h2_pal_webrtc_str_t){.data = state->offer, .len = state->offer_len},
      answer, sizeof(answer), &answer_len);
  if (exchange_result != 0) {
    printf("H2_WEBRTC_PERF stage=offer_exchange status=ERROR rc=%d\n",
           exchange_result);
    return -1;
  }
  const h2_pal_result_t remote_sdp_result = h2_pal_webrtc_peer_set_remote_sdp(
      state->api, state->peer, H2_PAL_WEBRTC_SDP_ANSWER,
      (h2_pal_webrtc_str_t){.data = answer, .len = answer_len});
  if (remote_sdp_result != H2_PAL_OK) {
    printf("H2_WEBRTC_PERF stage=remote_sdp status=ERROR rc=%d len=%zu\n",
           remote_sdp_result, answer_len);
    return -1;
  }
  const int connected_result =
      h2_webrtc_perf_wait(state, h2_webrtc_perf_connected, 20000u);
  if (connected_result != 0) {
    printf("H2_WEBRTC_PERF stage=connected_wait status=ERROR peer_state=%d "
           "open_mask=%u callback_error=%d callback_reason=%d\n",
           state->peer_state, state->persistent_open_mask,
           state->callback_error, state->callback_error_reason);
  }
  return connected_result;
}

int h2_webrtc_performance_run(h2_runtime_t *runtime,
                              const h2_webrtc_performance_config_t *config,
                              h2_webrtc_performance_result_t *out_result) {
  if (runtime == NULL || runtime->time == NULL || runtime->webrtc == NULL ||
      config == NULL || config->profile == NULL ||
      config->exchange_offer == NULL || out_result == NULL) {
    return 2;
  }
  memset(out_result, 0, sizeof(*out_result));
  const unsigned iterations = strcmp(config->profile, "smoke") == 0       ? 1u
                              : strcmp(config->profile, "benchmark") == 0 ? 10u
                              : strcmp(config->profile, "download") == 0  ? 1u
                                                                          : 0u;
  if (iterations == 0u) {
    return 2;
  }
  const int benchmark_profile = strcmp(config->profile, "benchmark") == 0;
  const uint64_t audio_gap_limit_ns =
      (uint64_t)(benchmark_profile ? H2_WEBRTC_PERF_AUDIO_LATE_GAP_MS +
                                         H2_WEBRTC_PERF_AUDIO_HOST_TOLERANCE_MS
                                   : H2_WEBRTC_PERF_AUDIO_SMOKE_MAX_GAP_MS) *
      UINT64_C(1000000);
  const unsigned audio_max_missing =
      benchmark_profile ? 0u : H2_WEBRTC_PERF_AUDIO_SMOKE_MAX_MISSING;
  int failed = 1;
  h2_webrtc_performance_state_t state = {
      .api = runtime->webrtc,
      .config = config,
      .runtime = runtime,
  };
  uint64_t idle_batch_samples[H2_WEBRTC_PERF_MAX_ITERATIONS] = {0};
  uint64_t loaded_batch_samples[H2_WEBRTC_PERF_MAX_ITERATIONS] = {0};
  uint64_t data_only_throughput_samples[H2_WEBRTC_PERF_MAX_ITERATIONS] = {0};
  uint64_t loaded_throughput_samples[H2_WEBRTC_PERF_MAX_ITERATIONS] = {0};
  uint64_t upload_throughput_samples[H2_WEBRTC_PERF_MAX_ITERATIONS] = {0};
  uint64_t download_throughput_samples[H2_WEBRTC_PERF_MAX_ITERATIONS] = {0};
  uint64_t fairness_samples[H2_WEBRTC_PERF_MAX_ITERATIONS] = {0};
  uint64_t audio_p99_gap_samples[H2_WEBRTC_PERF_MAX_ITERATIONS] = {0};
  uint64_t audio_gap_samples[H2_WEBRTC_PERF_MAX_ITERATIONS] = {0};
  if (h2_webrtc_perf_connect(&state) != 0) {
    printf("H2_WEBRTC_PERF setup failed\n");
    goto cleanup;
  }
  h2_webrtc_perf_checkpoint(&state, "connected");

  if (strcmp(config->profile, "download") == 0) {
    h2_webrtc_performance_transfer_t download = {0};
    if (h2_webrtc_perf_create_requests(&state, 1u) != 0 ||
        h2_webrtc_perf_run_transfer(&state, 0, 1, &download) != 0 ||
        h2_webrtc_perf_close_requests(&state) != 0) {
      printf("H2_WEBRTC_PERF download_only status=FAIL bytes=%zu "
             "callback=%d\n",
             state.download_bytes, state.callback_error);
      goto cleanup;
    }
    const uint64_t download_bytes_per_second =
        download.download_elapsed_ns == 0u
            ? 0u
            : (uint64_t)state.download_bytes * UINT64_C(1000000000) /
                  download.download_elapsed_ns;
    printf("H2_WEBRTC_PERF download_only status=PASS bytes=%zu "
           "elapsed_ms=%" PRIu64 " Bps=%" PRIu64 " poll_calls=%" PRIu64 "\n",
           state.download_bytes,
           download.download_elapsed_ns / UINT64_C(1000000),
           download_bytes_per_second, state.poll_calls);
    *out_result = (h2_webrtc_performance_result_t){
        .iterations = 1u,
        .median_download_bytes_per_second = download_bytes_per_second,
    };
    failed = 0;
    goto cleanup;
  }

  for (unsigned iteration = 0u; iteration < iterations; ++iteration) {
    const uint64_t poll_calls_before = state.poll_calls;
    if (h2_webrtc_perf_create_requests(&state, 3u) != 0 ||
        h2_webrtc_perf_send_requests(&state, 0u) != 0 ||
        h2_webrtc_perf_wait(&state, h2_webrtc_perf_requests_done, 10000u) !=
            0) {
      printf("H2_WEBRTC_PERF idle request batch failed\n");
      goto cleanup;
    }
    const uint64_t idle_batch_ns = state.request_batch_elapsed_ns;
    const uint64_t idle_max_rtt_ns = h2_webrtc_perf_request_max_rtt(&state);
    if (h2_webrtc_perf_close_requests(&state) != 0) {
      printf("H2_WEBRTC_PERF request lifecycle failed\n");
      goto cleanup;
    }
    h2_webrtc_performance_transfer_t data_only = {0};
    if (h2_webrtc_perf_create_requests(&state, 1u) != 0 ||
        h2_webrtc_perf_run_transfer(&state, 0, 0, &data_only) != 0 ||
        h2_webrtc_perf_close_requests(&state) != 0) {
      printf("H2_WEBRTC_PERF data-only transfer failed upload=%d/%zu "
             "download=%d/%zu callback=%d\n",
             state.upload_done, state.upload_bytes, state.download_done,
             state.download_bytes, state.callback_error);
      goto cleanup;
    }
    printf("H2_WEBRTC_PERF data_only status=PASS upload_bytes=%zu "
           "upload_elapsed_ms=%" PRIu64 " upload_Bps=%" PRIu64 " "
           "download_bytes=%zu download_elapsed_ms=%" PRIu64 " "
           "download_Bps=%" PRIu64 "\n",
           state.upload_bytes, data_only.upload_elapsed_ns / UINT64_C(1000000),
           data_only.upload_elapsed_ns == 0u
               ? 0u
               : (uint64_t)state.upload_bytes * UINT64_C(1000000000) /
                     data_only.upload_elapsed_ns,
           state.download_bytes,
           data_only.download_elapsed_ns / UINT64_C(1000000),
           data_only.download_elapsed_ns == 0u
               ? 0u
               : (uint64_t)state.download_bytes * UINT64_C(1000000000) /
                     data_only.download_elapsed_ns);
    if (h2_webrtc_perf_run_audio_only(&state) != 0) {
      printf("H2_WEBRTC_PERF audio-only failed received=%u late=%u "
             "deadline=%u callback=%d\n",
             state.audio_received, state.audio_late_gaps,
             state.audio_deadline_misses, state.callback_error);
      goto cleanup;
    }
    const uint64_t audio_only_p99_gap_ns = h2_webrtc_perf_audio_gap_p99(&state);
    const uint64_t audio_only_max_gap_ns = state.audio_max_gap_ns;
    const uint64_t audio_only_max_rtt_ns = state.audio_max_rtt_ns;
    const unsigned audio_only_received = state.audio_received;
    const unsigned audio_only_duplicates = state.audio_duplicates;
    const unsigned audio_only_reordered = state.audio_reordered;
    const unsigned audio_only_deadline_misses = state.audio_deadline_misses;
    const unsigned audio_only_would_block = state.audio_would_block;
    if (h2_webrtc_perf_create_requests(&state, 3u) != 0) {
      printf("H2_WEBRTC_PERF loaded request setup failed\n");
      goto cleanup;
    }
    h2_webrtc_performance_transfer_t loaded = {0};
    if (h2_webrtc_perf_run_transfer(&state, 1, 0, &loaded) != 0) {
      printf("H2_WEBRTC_PERF loaded failed upload=%d download=%d/%zu "
             "requests=0x%x audio=%u late=%u deadline=%u events=%u "
             "packet=%u max_gap_ns=%" PRIu64 " max_gap_sequence=%u "
             "callback=%d\n",
             state.upload_done, state.download_done, state.download_bytes,
             state.request_reply_mask, state.audio_received,
             state.audio_late_gaps, state.audio_deadline_misses,
             state.event_echoes, state.packet_echoes, state.audio_max_gap_ns,
             state.audio_max_gap_sequence, state.callback_error);
      goto cleanup;
    }
    const uint64_t loaded_batch_ns = state.request_batch_elapsed_ns;
    const uint64_t loaded_max_rtt_ns = h2_webrtc_perf_request_max_rtt(&state);
    const uint64_t data_only_throughput_bps =
        data_only.bulk_elapsed_ns == 0u
            ? 0u
            : (uint64_t)H2_WEBRTC_PERF_TRANSFER_SIZE * 2u * 1000000000u /
                  data_only.bulk_elapsed_ns;
    const uint64_t loaded_throughput_bps =
        loaded.bulk_elapsed_ns == 0u
            ? 0u
            : (uint64_t)H2_WEBRTC_PERF_TRANSFER_SIZE * 2u * 1000000000u /
                  loaded.bulk_elapsed_ns;
    const uint64_t upload_throughput_bps =
        loaded.upload_elapsed_ns == 0u
            ? 0u
            : (uint64_t)H2_WEBRTC_PERF_TRANSFER_SIZE * 1000000000u /
                  loaded.upload_elapsed_ns;
    const uint64_t download_throughput_bps =
        loaded.download_elapsed_ns == 0u
            ? 0u
            : (uint64_t)H2_WEBRTC_PERF_TRANSFER_SIZE * 1000000000u /
                  loaded.download_elapsed_ns;
    const uint64_t fairness_permille =
        loaded.bulk_elapsed_ns == 0u
            ? 0u
            : data_only.bulk_elapsed_ns * 1000u / loaded.bulk_elapsed_ns;
    const uint64_t loaded_audio_p99_gap_ns =
        h2_webrtc_perf_audio_gap_p99(&state);
    const unsigned audio_only_missing =
        audio_only_received >= H2_WEBRTC_PERF_AUDIO_FRAMES
            ? 0u
            : H2_WEBRTC_PERF_AUDIO_FRAMES - audio_only_received;
    const unsigned loaded_audio_missing =
        state.audio_received >= H2_WEBRTC_PERF_AUDIO_FRAMES
            ? 0u
            : H2_WEBRTC_PERF_AUDIO_FRAMES - state.audio_received;
    const int audio_only_ok =
        audio_only_p99_gap_ns <= audio_gap_limit_ns &&
        audio_only_missing <= audio_max_missing &&
        audio_only_duplicates == 0u && audio_only_reordered == 0u &&
        (!benchmark_profile ||
         (audio_only_deadline_misses == 0u && audio_only_would_block == 0u));
    const int loaded_audio_ok =
        loaded_audio_p99_gap_ns <= audio_gap_limit_ns &&
        loaded_audio_missing <= audio_max_missing &&
        state.audio_duplicates == 0u && state.audio_reordered == 0u &&
        (!benchmark_profile ||
         (state.audio_deadline_misses == 0u && state.audio_would_block == 0u));
    const int iteration_ok =
        fairness_permille >= 800u && audio_only_ok && loaded_audio_ok;
    printf("H2_WEBRTC_PERF iteration=%u requests=3 "
           "idle_batch_ns=%" PRIu64 " idle_max_rtt_ns=%" PRIu64 " "
           "loaded_batch_ns=%" PRIu64 " loaded_max_rtt_ns=%" PRIu64 " "
           "upload_bytes=%u download_bytes=%zu "
           "data_only_bulk_ns=%" PRIu64 " data_only_sequential_Bps=%" PRIu64 " "
           "loaded_bulk_ns=%" PRIu64 " loaded_sequential_Bps=%" PRIu64 " "
           "upload_elapsed_ns=%" PRIu64 " upload_Bps=%" PRIu64 " "
           "download_elapsed_ns=%" PRIu64 " download_Bps=%" PRIu64 " "
           "loaded_to_data_only_permille=%" PRIu64
           " audio_only_p99_gap_ns=%" PRIu64 " audio_only_max_gap_ns=%" PRIu64
           " audio_only_max_rtt_ns=%" PRIu64
           " audio_only_frames=%u audio_only_missing=%u "
           "audio_only_duplicates=%u audio_only_reordered=%u audio_frames=%u "
           "audio_max_gap_ns=%" PRIu64 " audio_max_rtt_ns=%" PRIu64
           " audio_late_gaps=%u audio_deadline_misses=%u "
           "audio_would_block=%u audio_missing=%u audio_duplicates=%u "
           "audio_reordered=%u events=%u packet=%u poll_calls=%" PRIu64 "\n",
           iteration, idle_batch_ns, idle_max_rtt_ns, loaded_batch_ns,
           loaded_max_rtt_ns, H2_WEBRTC_PERF_TRANSFER_SIZE,
           state.download_bytes, data_only.bulk_elapsed_ns,
           data_only_throughput_bps, loaded.bulk_elapsed_ns,
           loaded_throughput_bps, loaded.upload_elapsed_ns,
           upload_throughput_bps, loaded.download_elapsed_ns,
           download_throughput_bps, fairness_permille, audio_only_p99_gap_ns,
           audio_only_max_gap_ns, audio_only_max_rtt_ns, audio_only_received,
           audio_only_missing, audio_only_duplicates, audio_only_reordered,
           state.audio_received, state.audio_max_gap_ns, state.audio_max_rtt_ns,
           state.audio_late_gaps, state.audio_deadline_misses,
           state.audio_would_block, loaded_audio_missing,
           state.audio_duplicates, state.audio_reordered, state.event_echoes,
           state.packet_echoes, state.poll_calls - poll_calls_before);
    printf("{\"schema_version\":1,\"profile\":\"%s\","
           "\"target\":\"%s\",\"provider\":\"%s\","
           "\"transport\":\"udp\","
           "\"iteration\":%u,\"request_channels\":3,"
           "\"transfer_order\":\"download_then_upload\","
           "\"receive_mode\":\"pull\","
           "\"request_sids\":[%u,%u,%u],"
           "\"bytes_each_direction\":%u,"
           "\"idle_batch_ns\":%" PRIu64 ",\"loaded_batch_ns\":%" PRIu64
           ",\"data_only_sequential_Bps\":%" PRIu64
           ",\"loaded_sequential_Bps\":%" PRIu64 ",\"upload_Bps\":%" PRIu64
           ",\"download_Bps\":%" PRIu64
           ",\"loaded_to_data_only_permille\":%" PRIu64
           ",\"audio_budget_ms\":%u,\"audio_host_tolerance_ms\":%u"
           ",\"audio_gap_limit_ms\":%" PRIu64
           ",\"audio_only_p99_gap_ns\":%" PRIu64
           ",\"audio_only_max_gap_ns\":%" PRIu64
           ",\"audio_only_frames\":%u,\"audio_only_missing\":%u"
           ",\"audio_only_duplicates\":%u,\"audio_only_reordered\":%u"
           ",\"loaded_audio_p99_gap_ns\":%" PRIu64
           ",\"loaded_audio_max_gap_ns\":%" PRIu64
           ",\"loaded_audio_max_rtt_ns\":%" PRIu64
           ",\"loaded_audio_frames\":%u,\"loaded_audio_missing\":%u"
           ",\"loaded_audio_duplicates\":%u,\"loaded_audio_reordered\":%u"
           ",\"audio_late_gaps\":%u,\"audio_deadline_misses\":%u,"
           "\"audio_would_block\":%u,\"poll_calls\":%" PRIu64
           ",\"result\":\"%s\"}\n",
           config->profile, config->target == NULL ? "unknown" : config->target,
           config->provider == NULL ? "unknown" : config->provider, iteration,
           state.request_sids[0], state.request_sids[1], state.request_sids[2],
           H2_WEBRTC_PERF_TRANSFER_SIZE, idle_batch_ns, loaded_batch_ns,
           data_only_throughput_bps, loaded_throughput_bps,
           upload_throughput_bps, download_throughput_bps, fairness_permille,
           H2_WEBRTC_PERF_AUDIO_LATE_GAP_MS,
           H2_WEBRTC_PERF_AUDIO_HOST_TOLERANCE_MS,
           audio_gap_limit_ns / UINT64_C(1000000), audio_only_p99_gap_ns,
           audio_only_max_gap_ns, audio_only_received, audio_only_missing,
           audio_only_duplicates, audio_only_reordered, loaded_audio_p99_gap_ns,
           state.audio_max_gap_ns, state.audio_max_rtt_ns, state.audio_received,
           loaded_audio_missing, state.audio_duplicates, state.audio_reordered,
           state.audio_late_gaps, state.audio_deadline_misses,
           state.audio_would_block, state.poll_calls - poll_calls_before,
           iteration_ok ? "pass" : "fail");
    idle_batch_samples[iteration] = idle_batch_ns;
    loaded_batch_samples[iteration] = loaded_batch_ns;
    data_only_throughput_samples[iteration] = data_only_throughput_bps;
    loaded_throughput_samples[iteration] = loaded_throughput_bps;
    upload_throughput_samples[iteration] = upload_throughput_bps;
    download_throughput_samples[iteration] = download_throughput_bps;
    fairness_samples[iteration] = fairness_permille;
    audio_p99_gap_samples[iteration] = h2_webrtc_perf_audio_gap_p99(&state);
    audio_gap_samples[iteration] = state.audio_max_gap_ns;
    if (!audio_only_ok) {
      printf("H2_WEBRTC_PERF audio-only performance gate failed p99=%" PRIu64
             " frames=%u missing=%u duplicates=%u reordered=%u "
             "deadline=%u would_block=%u\n",
             audio_only_p99_gap_ns, audio_only_received, audio_only_missing,
             audio_only_duplicates, audio_only_reordered,
             audio_only_deadline_misses, audio_only_would_block);
      goto cleanup;
    }
    if (!loaded_audio_ok) {
      printf("H2_WEBRTC_PERF loaded audio scheduling gate failed frames=%u "
             "missing=%u duplicates=%u reordered=%u deadline=%u "
             "would_block=%u\n",
             state.audio_received, loaded_audio_missing, state.audio_duplicates,
             state.audio_reordered, state.audio_deadline_misses,
             state.audio_would_block);
      goto cleanup;
    }
    if (h2_webrtc_perf_close_requests(&state) != 0) {
      goto cleanup;
    }
  }
  const uint64_t median_fairness =
      h2_webrtc_perf_median(fairness_samples, iterations);
  if (median_fairness < 800u) {
    printf("H2_WEBRTC_PERF median loaded throughput below 80%% of "
           "data-only throughput\n");
    goto cleanup;
  }
  printf("H2_WEBRTC_PERF summary iterations=%u median_idle_batch_ns=%" PRIu64
         " median_loaded_batch_ns=%" PRIu64
         " median_data_only_sequential_Bps=%" PRIu64
         " median_loaded_sequential_Bps=%" PRIu64 " median_upload_Bps=%" PRIu64
         " median_download_Bps=%" PRIu64
         " median_loaded_to_data_only_permille=%" PRIu64
         " median_audio_p99_gap_ns=%" PRIu64 " median_audio_max_gap_ns=%" PRIu64
         "\n",
         iterations, h2_webrtc_perf_median(idle_batch_samples, iterations),
         h2_webrtc_perf_median(loaded_batch_samples, iterations),
         h2_webrtc_perf_median(data_only_throughput_samples, iterations),
         h2_webrtc_perf_median(loaded_throughput_samples, iterations),
         h2_webrtc_perf_median(upload_throughput_samples, iterations),
         h2_webrtc_perf_median(download_throughput_samples, iterations),
         median_fairness,
         h2_webrtc_perf_median(audio_p99_gap_samples, iterations),
         h2_webrtc_perf_median(audio_gap_samples, iterations));
  *out_result = (h2_webrtc_performance_result_t){
      .iterations = iterations,
      .median_upload_bytes_per_second =
          h2_webrtc_perf_median(upload_throughput_samples, iterations),
      .median_download_bytes_per_second =
          h2_webrtc_perf_median(download_throughput_samples, iterations),
      .median_loaded_bytes_per_second =
          h2_webrtc_perf_median(loaded_throughput_samples, iterations),
      .median_request_batch_ns =
          h2_webrtc_perf_median(loaded_batch_samples, iterations),
      .median_audio_p99_gap_ns =
          h2_webrtc_perf_median(audio_p99_gap_samples, iterations),
      .median_audio_max_gap_ns =
          h2_webrtc_perf_median(audio_gap_samples, iterations),
  };
  failed = 0;

cleanup:
  if (state.peer != NULL) {
    h2_pal_webrtc_peer_close(state.api, state.peer);
  }
  return failed;
}
