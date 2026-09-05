#ifndef H2_PEER_INTERNAL_H
#define H2_PEER_INTERNAL_H

#include "h2_peer.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define H2_PEER_ICE_SERVER_MAX 5u
#define H2_PEER_CHANNEL_LABEL_MAX 128u
#define H2_PEER_READY_CHANNEL_COUNT 32u
#define H2_PEER_LOCAL_STREAM_COUNT 150u
#define H2_PEER_STREAM_COUNT (H2_PEER_LOCAL_STREAM_COUNT * 2u)
#define H2_PEER_WIRE_PACKET_MAX 1500u
#define H2_PEER_CHANNEL_FREE_PENDING UINT32_C(0x80000000)
#define H2_PEER_MEDIA_RECEIVE_LIMIT 64u

typedef struct h2_peer_media_frame h2_peer_media_frame_t;

typedef struct h2_peer_ice_server {
  char *url;
  char *username;
  char *credential;
} h2_peer_ice_server_t;

typedef struct h2_peer_stream_reset {
  uint32_t generation;
  uint8_t active;
  uint8_t outgoing_submitted;
  uint8_t outgoing_completed;
  uint8_t incoming_reset;
} h2_peer_stream_reset_t;

typedef struct h2_peer_tx_item {
  uint8_t *data;
  size_t len;
  size_t capacity;
  int is_text;
} h2_peer_tx_item_t;

enum {
  /* Per-channel outbound ring depth. A producer may queue this many messages
   * ahead of the network task, so a bulk sender no longer stalls for one
   * WRITABLE round trip per message. */
  H2_PEER_INPUT_SLOT_COUNT = 4,
  /* Per-round channel send budget. One network round sends queued channel
   * messages until either bound is reached, then returns so RTP audio, the
   * receive pump and the next round's writable check run before more bulk
   * data goes out. At least one message is always sent when one is ready. */
  H2_PEER_NETWORK_CHANNEL_ROUND_BYTES = 8192,
  H2_PEER_NETWORK_CHANNEL_ROUND_MESSAGES = 8,
};

typedef struct h2_peer_round_stats {
  uint64_t count;
  uint64_t total_us;
  uint64_t max_us;
} h2_peer_round_stats_t;

struct h2_pal_webrtc_channel {
  struct h2_pal_webrtc_peer *owner;
  struct h2_pal_webrtc_channel *next;
  h2_pal_webrtc_channel_info_t info;
  char *label;
  uint32_t generation;
  atomic_int open;
  int wire_opened;
  int remote_created;
  atomic_int terminal;
  atomic_uint event_refs;
  atomic_uchar ready_slot;
  /* Ring of queued messages: producers fill at tx_tail, the network task
   * drains from tx_head. tx_state per slot: 0 free, 1 filling, 2 ready. */
  h2_peer_tx_item_t *tx_storage[H2_PEER_INPUT_SLOT_COUNT];
  atomic_uchar tx_state[H2_PEER_INPUT_SLOT_COUNT];
  atomic_uchar tx_head;
  atomic_uchar tx_tail;
  atomic_uint_fast64_t tx_ready_since_us;
};

struct h2_pal_webrtc_peer {
  h2_peer_t *owner;
  struct h2_pal_webrtc_peer *next;
  h2_pal_webrtc_track_t *media_track;
  uint8_t media_pending_opus[H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE];
  size_t media_pending_opus_len;
  h2_peer_media_frame_t *media_receive_head;
  h2_peer_media_frame_t *media_receive_tail;
  size_t media_receive_count;
  h2_pal_webrtc_channel_t *channels;
  h2_peer_ice_server_t ice_servers[H2_PEER_ICE_SERVER_MAX];
  size_t ice_server_count;
  _Atomic(h2_pal_webrtc_peer_state_t) state;
  h2_peer_stream_reset_t stream_resets[H2_PEER_STREAM_COUNT];
  uint16_t next_stream_id;
  uint16_t local_stream_first;
  h2_pal_result_t stream_reset_failure;
  void *production_pc;
  int offer_started;
  int remote_answer_set;
  int production_sctp_open;
  atomic_int closed;
  /* One live handle/worker reference, plus one per owned event. */
  atomic_uint refs;
  h2_pal_queue_t *network_commands;
  h2_pal_queue_t *network_responses;
  h2_pal_queue_t *network_events;
  h2_pal_mutex_t *network_request_mutex;
  h2_pal_task_t *network_task;
  atomic_uint network_event_count;
  atomic_size_t network_event_bytes;
  atomic_int network_send_wakeup_queued;
  atomic_int network_stop;
  atomic_int network_stopped;
  /* One caller at a time may sit in peer_poll; close waits for it to leave
   * before destroying the queues that poll is receiving from. */
  atomic_int network_poll_active;
  atomic_int network_transport_result;
  atomic_int network_error_reported;
  _Atomic(h2_peer_tx_item_t *) rtp_pending;
  atomic_uint_fast64_t rtp_ready_since_us;
  h2_peer_tx_item_t *rtp_storage;
  _Atomic(uint32_t) channel_ready;
  uint8_t channel_round_robin;
  uint64_t perf_window_started_us;
  h2_peer_round_stats_t perf_command;
  h2_peer_round_stats_t perf_send;
  h2_peer_round_stats_t perf_transport;
  h2_peer_round_stats_t perf_idle;
  atomic_uint perf_rtp_enqueue_blocked;
  uint64_t perf_rtp_service_count;
  uint64_t perf_rtp_send_blocked;
  uint64_t perf_rtp_cleared;
  uint64_t perf_rtp_max_ready_us;
  uint64_t perf_channel_service_count;
  uint64_t perf_channel_send_blocked;
  uint64_t perf_channel_max_ready_us;
  uint64_t perf_media_reads;
  uint64_t perf_media_empty;
  uint64_t perf_media_receive_dropped;
  uint64_t media_rx_window_started_us;
  uint64_t media_rx_last_arrival_us;
  uint64_t media_rx_interval_min_us;
  uint64_t media_rx_interval_max_us;
  uint64_t media_rx_interval_total_us;
  uint64_t media_rx_interval_count;
  uint64_t media_rx_received_frames;
  uint64_t media_rx_dropped_frames;
  uint64_t media_rx_sequence_discontinuities;
  uint64_t media_rx_timestamp_discontinuities;
  uint32_t media_rx_last_timestamp;
  uint16_t media_rx_last_sequence;
  uint16_t media_rx_burst_1ms_current;
  uint16_t media_rx_burst_1ms_max;
  uint16_t media_rx_burst_5ms_current;
  uint16_t media_rx_burst_5ms_max;
  size_t media_rx_queue_max;
  uint8_t media_rx_metadata_initialized;
  uint64_t media_track_window_started_us;
  uint64_t media_track_write_ok;
  uint64_t media_track_write_would_block;
  uint64_t media_track_block_started_us;
  uint64_t media_track_blocked_max_us;
  uint64_t media_track_rounds;
  uint64_t media_track_round_frames_total;
  size_t media_track_round_frames_last;
  size_t media_track_round_frames_max;
};

struct h2_peer {
  h2_peer_config_t config;
  h2_pal_webrtc_api_t webrtc_api;
  h2_pal_webrtc_peer_t *peers;
  /* One public owner reference, plus one per allocated peer. */
  atomic_uint refs;
  int destroying;
};

void h2_peer_webrtc_on_stream_reset(
    h2_pal_webrtc_peer_t *peer, const h2_pal_sctp_stream_reset_event_t *event);

void h2_peer_webrtc_on_sctp_closed(h2_pal_webrtc_peer_t *peer);

h2_pal_result_t h2_peer_webrtc_on_remote_channel_open(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_str_t label, uint16_t stream_id,
    int ordered, int reliable);

void h2_peer_webrtc_emit_peer_state(h2_pal_webrtc_peer_t *peer,
                                    h2_pal_webrtc_peer_state_t state);

void h2_peer_webrtc_emit_channel_state(h2_pal_webrtc_channel_t *channel,
                                       h2_pal_webrtc_channel_state_t state);

void h2_peer_webrtc_emit_local_sdp(h2_pal_webrtc_peer_t *peer,
                                   h2_pal_webrtc_sdp_type_t type,
                                   h2_pal_webrtc_str_t sdp);

h2_pal_result_t h2_peer_webrtc_emit_channel_message(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_channel_t *channel,
    const uint8_t *data, size_t len, int is_text);

void h2_peer_webrtc_emit_opus_frame(h2_pal_webrtc_peer_t *peer,
                                    const uint8_t *opus, size_t opus_len);
void h2_peer_webrtc_note_audio_rtp(h2_pal_webrtc_peer_t *peer,
                                   uint16_t sequence, uint32_t timestamp);

/* Network-task-owned media pump/cleanup; not part of the public PAL API. */
h2_pal_result_t h2_peer_webrtc_service_media(h2_pal_webrtc_peer_t *peer);
void h2_peer_webrtc_discard_media(h2_pal_webrtc_peer_t *peer);

/* Channel TX ring and its network-round scheduler; exposed for unit tests. */
h2_pal_result_t h2_peer_channel_tx_push(h2_pal_webrtc_channel_t *channel,
                                        const uint8_t *data, size_t len,
                                        int is_text);
int h2_peer_network_service_channel(h2_pal_webrtc_peer_t *peer,
                                    uint32_t *snapshot);

/* Network-task-owned media pump/cleanup; not part of the public PAL API. */
h2_pal_result_t h2_peer_webrtc_service_media(h2_pal_webrtc_peer_t *peer);
void h2_peer_webrtc_discard_media(h2_pal_webrtc_peer_t *peer);

/* Channel TX ring and its network-round scheduler; exposed for unit tests. */
h2_pal_result_t h2_peer_channel_tx_push(h2_pal_webrtc_channel_t *channel,
                                        const uint8_t *data, size_t len,
                                        int is_text);
int h2_peer_network_service_channel(h2_pal_webrtc_peer_t *peer,
                                    uint32_t *snapshot);

#endif
