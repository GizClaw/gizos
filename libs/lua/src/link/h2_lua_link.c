#include "../runtime/h2_lua_internal.h"
#include "h2_bleikcp.h"
#include "h2_lua_link.h"
#include "h2_lua_link_task_names.h"
#include "h2/pal/h2_pal_unsupported.h"

#include <stddef.h>
#include <string.h>

#define H2_LUA_LINK_EVENT_CAPACITY 16u
/* Datagrams can overtake the peer's HELLO, which travels over KCP; this many
 * are held until the handshake completes and then follow LINK_CONNECTED. */
#define H2_LUA_LINK_EARLY_DATAGRAMS 4u
#define H2_LUA_LINK_PROTOCOL_VERSION 1u
/* KCP frames: [type u8][len u16 big-endian][payload]. */
#define H2_LUA_LINK_FRAME_HEADER 3u
#define H2_LUA_LINK_FRAME_HELLO 1u
#define H2_LUA_LINK_FRAME_BYE 2u
#define H2_LUA_LINK_FRAME_MESSAGE 3u
#define H2_LUA_LINK_FRAME_STREAM 4u
#define H2_LUA_LINK_STREAM_CHUNK 512u
#define H2_LUA_LINK_FRAME_MAX                                                  \
  (H2_LUA_LINK_FRAME_HEADER + H2_LUA_LINK_STREAM_CHUNK)
#define H2_LUA_LINK_HELLO_TIMEOUT_MS 5000u
#define H2_LUA_LINK_BYE_FLUSH_MS 400u
#define H2_LUA_LINK_HANDLER_EXIT_MS 1000u
_Static_assert(2u * H2_LUA_LINK_BYE_FLUSH_MS < H2_LUA_LINK_HANDLER_EXIT_MS,
               "the host must wait out a BYE write and flush before closing");
#define H2_LUA_LINK_SLICE_MS 50u
#define H2_LUA_LINK_READ_POLL_MS 2u
#define H2_LUA_LINK_JOIN_TIMEOUT_DEFAULT_MS 10000u
#define H2_LUA_LINK_JOIN_TIMEOUT_MAX_MS 60000u
#define H2_LUA_LINK_CONNECT_TIMEOUT_MS 5000u
#define H2_LUA_LINK_SUPERVISION_TIMEOUT_MS 2000u
#define H2_LUA_LINK_CONN_INTERVAL_MS 30u
#define H2_LUA_LINK_ADV_INTERVAL_MIN_MS 100u
#define H2_LUA_LINK_ADV_INTERVAL_MAX_MS 150u
#define H2_LUA_LINK_ADV_SID 3u
#define H2_LUA_LINK_SCAN_INTERVAL_MS 50u
#define H2_LUA_LINK_KCP_DATAGRAM_MAX 244u
#define H2_LUA_LINK_KCP_WINDOW 16u
#define H2_LUA_LINK_KCP_INPUT_FRAMES 32u
#define H2_LUA_LINK_KCP_BUFFER_SIZE 4096u
#define H2_LUA_LINK_KCP_CONV UINT32_C(0x4c4e4b31)
#define H2_LUA_LINK_TASK_STACK_SIZE (8u * 1024u)
#define H2_LUA_LINK_UUID_LEN 16u

_Static_assert(H2_LUA_LINK_FRAME_MAX <= H2_LUA_LINK_KCP_BUFFER_SIZE,
               "a maximum-size frame must fit the KCP send buffer");
_Static_assert(H2_LUA_LINK_EARLY_DATAGRAMS + 1u <= H2_LUA_LINK_EVENT_CAPACITY,
               "CONNECTED and the early datagrams must fit an empty ring");
_Static_assert(H2_LUA_LINK_STREAM_BUFFER_SIZE >= H2_LUA_LINK_STREAM_CHUNK,
               "one stream frame must fit the receive buffer");

/*
 * One fixed GATT service for every link, so a BLE Host whose GATT table only
 * grows (ESP NimBLE) registers it once. UUID bytes are little-endian, as the
 * PAL expects:
 *   service  0685b801-18da-449c-88a2-66c491b17772
 *   KCP TX   0685b802-18da-449c-88a2-66c491b17772 (notify, bleikcp)
 *   KCP RX   0685b803-18da-449c-88a2-66c491b17772 (write, bleikcp)
 *   datagram 0685b804-18da-449c-88a2-66c491b17772 (write-no-rsp, notify)
 */
#define H2_LUA_LINK_GATT_UUID(n)                                               \
  {0x72u, 0x77u, 0xb1u, 0x91u, 0xc4u, 0x66u, 0xa2u, 0x88u,                       \
   0x9cu, 0x44u, 0xdau, 0x18u, (n), 0xb8u, 0x85u, 0x06u}
static const uint8_t s_service_uuid[H2_LUA_LINK_UUID_LEN] =
    H2_LUA_LINK_GATT_UUID(0x01u);
static const uint8_t s_tx_uuid[H2_LUA_LINK_UUID_LEN] =
    H2_LUA_LINK_GATT_UUID(0x02u);
static const uint8_t s_rx_uuid[H2_LUA_LINK_UUID_LEN] =
    H2_LUA_LINK_GATT_UUID(0x03u);
static const uint8_t s_datagram_uuid[H2_LUA_LINK_UUID_LEN] =
    H2_LUA_LINK_GATT_UUID(0x04u);
/* Advertised session UUID 0221d1f2-9dce-4921-bac4-eeb9XXXXXXXX: the last four
 * bytes are the FNV-1a hash of the tag. It only appears in advertising, never
 * in the GATT table. */
static const uint8_t s_adv_base[H2_LUA_LINK_UUID_LEN] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0xb9u, 0xeeu, 0xc4u, 0xbau,
    0x21u, 0x49u, 0xceu, 0x9du, 0xf2u, 0xd1u, 0x21u, 0x02u,
};
static const uint8_t s_cccd_uuid[] = {0x02u, 0x29u};

typedef enum h2_lua_link_state {
  H2_LUA_LINK_IDLE = 0,
  H2_LUA_LINK_HOSTING,
  H2_LUA_LINK_JOINING,
  H2_LUA_LINK_CONNECTED,
} h2_lua_link_state_t;

typedef enum h2_lua_link_role {
  H2_LUA_LINK_ROLE_HOST = 0,
  H2_LUA_LINK_ROLE_JOIN,
} h2_lua_link_role_t;

/* How one stream ended, before it is mapped to a Lua event. */
typedef enum h2_lua_link_outcome {
  H2_LUA_LINK_OUTCOME_LOCAL = 0,
  H2_LUA_LINK_OUTCOME_SETUP_FAILED,
  H2_LUA_LINK_OUTCOME_SETUP_TIMEOUT,
  H2_LUA_LINK_OUTCOME_MISMATCH,
  H2_LUA_LINK_OUTCOME_PEER_CLOSED,
  H2_LUA_LINK_OUTCOME_LOST,
} h2_lua_link_outcome_t;

typedef struct h2_lua_link_event {
  uint32_t kind;
  uint64_t sequence;
  uint64_t timestamp_ms;
  h2_lua_link_role_t role;
  const char *reason;
  int result;
  int reliable;
  size_t max_datagram;
  size_t len;
  uint8_t data[H2_LUA_LINK_MESSAGE_MAX];
} h2_lua_link_event_t;

typedef struct h2_lua_link_reader {
  uint8_t data[H2_LUA_LINK_FRAME_MAX];
  size_t len;
} h2_lua_link_reader_t;

typedef struct h2_lua_link {
  h2_runtime_t *runtime;
  h2_lua_link_config_t config;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *cond;

  /* Session slot: in_use from a successful host/join until the next
   * host/join or destroy joins the finished task. */
  int in_use;
  int task_done;
  /* The session queued its final DISCONNECTED or ERROR event; only
   * bookkeeping remains before task_done. */
  int ended;
  h2_pal_task_t *task;
  h2_lua_job_t *job;
  h2_lua_job_id_t job_id;
  uint32_t job_generation;
  h2_lua_link_role_t role;
  h2_lua_link_state_t state;
  int closing;
  uint32_t timeout_ms;
  uint64_t started_ms;
  uint8_t tag[H2_LUA_LINK_TAG_MAX];
  size_t tag_len;
  uint8_t adv_uuid[H2_LUA_LINK_UUID_LEN];
  /* Published for send()/write() while connected; cleared under mutex before
   * the owning reader releases the stream. */
  h2_bleikcp_t *stream;
  /* Datagram path of the connected peer: the host notifies its own value
   * handle, the joiner writes the peer's. Valid while CONNECTED. */
  uint16_t conn_handle;
  uint16_t datagram_handle;
  size_t max_datagram;
  uint64_t datagrams_dropped;
  /* Host-side datagram characteristic, borrowed by the bleikcp server. */
  h2_pal_ble_gatt_characteristic_t datagram_characteristic;
  uint16_t datagram_value_handle;
  uint16_t datagram_cccd_handle;
  uint8_t early_datagrams[H2_LUA_LINK_EARLY_DATAGRAMS]
                        [H2_LUA_LINK_UNRELIABLE_MAX];
  size_t early_datagram_len[H2_LUA_LINK_EARLY_DATAGRAMS];
  size_t early_datagram_count;
  /* Stream bytes received in STREAM frames and not yet read by Lua. */
  uint8_t stream_rx[H2_LUA_LINK_STREAM_BUFFER_SIZE];
  size_t stream_rx_head;
  size_t stream_rx_len;
  int peer_attached;
  int handler_done;
  h2_lua_link_outcome_t handler_outcome;
  int handler_result;
  int scan_found;
  h2_pal_ble_addr_t scan_addr;

  h2_lua_link_event_t events[H2_LUA_LINK_EVENT_CAPACITY];
  size_t event_head;
  size_t event_count;
  size_t next_callback_index;
  uint64_t next_sequence;
} h2_lua_link_t;

static uint64_t link_now_ms(const h2_lua_link_t *link) {
  uint64_t now = 0u;
  (void)h2_pal_time_get_monotonic_ms(link->runtime->time, &now);
  return now;
}

static void link_lock(h2_lua_link_t *link) {
  (void)h2_pal_mutex_lock(link->runtime->sync, link->mutex);
}

static void link_unlock(h2_lua_link_t *link) {
  (void)h2_pal_mutex_unlock(link->runtime->sync, link->mutex);
}

static void link_wait(h2_lua_link_t *link, uint32_t timeout_ms) {
  (void)h2_pal_cond_wait(link->runtime->sync, link->cond, link->mutex,
                         timeout_ms);
}

static void link_broadcast(h2_lua_link_t *link) {
  (void)h2_pal_cond_broadcast(link->runtime->sync, link->cond);
}

static h2_bleikcp_api_t link_bleikcp_api(const h2_lua_link_t *link) {
  return (h2_bleikcp_api_t){
      .ble = link->runtime->ble_host,
      .task = link->runtime->task,
      .time = link->runtime->time,
      .sync = link->runtime->sync,
      .system_event = link->runtime->system_event,
      .allocator = link->runtime->mem,
  };
}

static h2_bleikcp_config_t link_bleikcp_config(const h2_lua_link_t *link) {
  return (h2_bleikcp_config_t){
      .service_uuid = {s_service_uuid, sizeof(s_service_uuid)},
      .tx_char_uuid = {s_tx_uuid, sizeof(s_tx_uuid)},
      .rx_char_uuid = {s_rx_uuid, sizeof(s_rx_uuid)},
      .conv = H2_LUA_LINK_KCP_CONV,
      .max_datagram_len = H2_LUA_LINK_KCP_DATAGRAM_MAX,
      .send_window = H2_LUA_LINK_KCP_WINDOW,
      .recv_window = H2_LUA_LINK_KCP_WINDOW,
      .input_frame_capacity = H2_LUA_LINK_KCP_INPUT_FRAMES,
      .tx_buffer_size = H2_LUA_LINK_KCP_BUFFER_SIZE,
      .rx_buffer_size = H2_LUA_LINK_KCP_BUFFER_SIZE,
      .no_congestion_control = 1,
      .setup_timeout_ms = H2_LUA_LINK_CONNECT_TIMEOUT_MS,
      .output_retry_count = 40u,
      .output_retry_delay_ms = 2u,
      .worker_task_options = {NULL, H2_LUA_LINK_TASK_STACK_SIZE},
      .server_task_options = {NULL, H2_LUA_LINK_TASK_STACK_SIZE},
      .extra_characteristics = link->role == H2_LUA_LINK_ROLE_HOST
                                   ? &link->datagram_characteristic
                                   : NULL,
      .extra_characteristic_count = link->role == H2_LUA_LINK_ROLE_HOST,
  };
}

static void link_make_adv_uuid(const uint8_t *tag, size_t tag_len,
                               uint8_t out[H2_LUA_LINK_UUID_LEN]) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0u; i < tag_len; ++i) {
    hash ^= tag[i];
    hash *= 16777619u;
  }
  memcpy(out, s_adv_base, H2_LUA_LINK_UUID_LEN);
  out[0] = (uint8_t)hash;
  out[1] = (uint8_t)(hash >> 8u);
  out[2] = (uint8_t)(hash >> 16u);
  out[3] = (uint8_t)(hash >> 24u);
}

static int link_session_owned_by(const h2_lua_link_t *link,
                                 h2_lua_job_id_t job_id,
                                 uint32_t job_generation) {
  return link->in_use && link->job_id == job_id &&
         link->job_generation == job_generation;
}

/* Appends one event with the common fields filled; NULL when the ring is
 * full. Called with the link mutex held. */
static h2_lua_link_event_t *link_push_locked(h2_lua_link_t *link,
                                             uint32_t kind) {
  h2_lua_link_event_t *event;
  if (link->event_count == H2_LUA_LINK_EVENT_CAPACITY) {
    return NULL;
  }
  event = &link->events[(link->event_head + link->event_count) %
                        H2_LUA_LINK_EVENT_CAPACITY];
  memset(event, 0, offsetof(h2_lua_link_event_t, data));
  event->kind = kind;
  event->sequence = ++link->next_sequence;
  event->timestamp_ms = link_now_ms(link);
  event->role = link->role;
  event->max_datagram = link->max_datagram;
  link->event_count++;
  return event;
}

/* Queues one event for the owning job. Blocks while the ring is full so a
 * slow Lua consumer back-pressures the peer through the KCP window. */
static int link_post(h2_lua_link_t *link, uint32_t kind, const char *reason,
                     int result, const uint8_t *data, size_t len) {
  h2_lua_link_event_t *event;
  link_lock(link);
  while (!link->closing && link->event_count == H2_LUA_LINK_EVENT_CAPACITY) {
    link_wait(link, H2_LUA_LINK_SLICE_MS);
  }
  if (link->closing) {
    link_unlock(link);
    return H2_PAL_ERR_CLOSED;
  }
  event = link_push_locked(link, kind);
  if (kind == H2_LUA_LINK_EVENT_DISCONNECTED ||
      kind == H2_LUA_LINK_EVENT_ERROR) {
    link->ended = 1;
  }
  event->reason = reason;
  event->result = result;
  event->reliable = 1;
  event->len = len;
  if (len != 0u) {
    memcpy(event->data, data, len);
  }
  /* Wake under the link mutex: job_ended() sets closing under it, with the
   * job mutex held, before the job slot is released or reused. */
  h2_lua_host_wake_job(link->job);
  link_unlock(link);
  return H2_PAL_OK;
}

static void link_post_outcome(h2_lua_link_t *link,
                              h2_lua_link_outcome_t outcome, int result) {
  switch (outcome) {
  case H2_LUA_LINK_OUTCOME_SETUP_FAILED:
    (void)link_post(link, H2_LUA_LINK_EVENT_ERROR, "ble", result, NULL, 0u);
    break;
  case H2_LUA_LINK_OUTCOME_SETUP_TIMEOUT:
    (void)link_post(link, H2_LUA_LINK_EVENT_ERROR, "timeout",
                    H2_PAL_ERR_TIMEOUT, NULL, 0u);
    break;
  case H2_LUA_LINK_OUTCOME_MISMATCH:
    (void)link_post(link, H2_LUA_LINK_EVENT_ERROR, "mismatch",
                    H2_PAL_ERR_FORMAT, NULL, 0u);
    break;
  case H2_LUA_LINK_OUTCOME_PEER_CLOSED:
    (void)link_post(link, H2_LUA_LINK_EVENT_DISCONNECTED, "peer_closed",
                    H2_PAL_ERR_CLOSED, NULL, 0u);
    break;
  case H2_LUA_LINK_OUTCOME_LOST:
    (void)link_post(link, H2_LUA_LINK_EVENT_DISCONNECTED, "lost", result,
                    NULL, 0u);
    break;
  case H2_LUA_LINK_OUTCOME_LOCAL:
  default:
    break;
  }
}

/* Unreliable input from a GATT callback or system event: never blocks the
 * BLE Host, drops when the session is not connected or the ring is full. */
static void link_datagram_input(h2_lua_link_t *link, uint16_t conn_handle,
                                uint16_t attr_handle, const uint8_t *data,
                                size_t len) {
  h2_lua_link_event_t *event;
  link_lock(link);
  if (link->closing || conn_handle != link->conn_handle ||
      attr_handle != link->datagram_handle ||
      attr_handle == H2_PAL_BLE_INVALID_ATTR_HANDLE || len == 0u ||
      len > H2_LUA_LINK_UNRELIABLE_MAX) {
    link->datagrams_dropped++;
  } else if (link->state != H2_LUA_LINK_CONNECTED) {
    if (link->early_datagram_count < H2_LUA_LINK_EARLY_DATAGRAMS) {
      memcpy(link->early_datagrams[link->early_datagram_count], data, len);
      link->early_datagram_len[link->early_datagram_count++] = len;
    } else {
      link->datagrams_dropped++;
    }
  } else if ((event = link_push_locked(link, H2_LUA_LINK_EVENT_MESSAGE)) ==
             NULL) {
    link->datagrams_dropped++;
  } else {
    event->len = len;
    memcpy(event->data, data, len);
    h2_lua_host_wake_job(link->job);
  }
  link_unlock(link);
}

static h2_pal_result_t link_datagram_server_write(
    void *user, const h2_pal_ble_gatt_access_t *access, const uint8_t *data,
    size_t len) {
  h2_lua_link_t *link = user;
  if (access == NULL || (len != 0u && data == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  link_datagram_input(link, access->conn_handle, access->attr_handle, data,
                      len);
  return H2_PAL_OK;
}

static int link_datagram_client_event(void *user,
                                      const h2_pal_system_event_t *event) {
  h2_lua_link_t *link = user;
  const h2_pal_ble_gatt_client_value_t *value;
  if (event == NULL ||
      event->type != H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION ||
      event->payload_size != sizeof(h2_pal_ble_gatt_client_value_t)) {
    return H2_PAL_OK;
  }
  value = event->payload;
  if (value->value_len <= sizeof(value->value)) {
    link_datagram_input(link, value->conn_handle, value->attr_handle,
                        value->value, value->value_len);
  }
  return H2_PAL_OK;
}

/* Moves one STREAM frame into the Lua read buffer, waiting for space so an
 * unread stream back-pressures the peer through the KCP window. */
static int link_stream_input(h2_lua_link_t *link, const uint8_t *data,
                             size_t len) {
  link_lock(link);
  while (!link->closing &&
         H2_LUA_LINK_STREAM_BUFFER_SIZE - link->stream_rx_len < len) {
    link_wait(link, H2_LUA_LINK_SLICE_MS);
  }
  if (link->closing) {
    link_unlock(link);
    return H2_PAL_ERR_CLOSED;
  }
  for (size_t i = 0u; i < len; ++i) {
    link->stream_rx[(link->stream_rx_head + link->stream_rx_len + i) %
                    H2_LUA_LINK_STREAM_BUFFER_SIZE] = data[i];
  }
  link->stream_rx_len += len;
  h2_lua_host_wake_job(link->job);
  link_unlock(link);
  return H2_PAL_OK;
}

static void link_reset_buffers_locked(h2_lua_link_t *link) {
  link->early_datagram_count = 0u;
  link->event_head = 0u;
  link->event_count = 0u;
  link->next_callback_index = 0u;
  link->stream_rx_head = 0u;
  link->stream_rx_len = 0u;
}

static int link_is_closing(h2_lua_link_t *link) {
  int closing;
  link_lock(link);
  closing = link->closing;
  link_unlock(link);
  return closing;
}

static int link_write_frame(h2_bleikcp_t *stream, uint8_t type,
                            const uint8_t *prefix, size_t prefix_len,
                            const uint8_t *payload, size_t payload_len,
                            uint32_t timeout_ms) {
  uint8_t frame[H2_LUA_LINK_FRAME_MAX];
  size_t len = prefix_len + payload_len;
  if (len > H2_LUA_LINK_STREAM_CHUNK) {
    return H2_PAL_ERR_NO_SPACE;
  }
  frame[0] = type;
  frame[1] = (uint8_t)(len >> 8u);
  frame[2] = (uint8_t)len;
  if (prefix_len != 0u) {
    memcpy(frame + H2_LUA_LINK_FRAME_HEADER, prefix, prefix_len);
  }
  if (payload_len != 0u) {
    memcpy(frame + H2_LUA_LINK_FRAME_HEADER + prefix_len, payload,
           payload_len);
  }
  return h2_bleikcp_write(stream, frame, H2_LUA_LINK_FRAME_HEADER + len,
                          timeout_ms);
}

/* Returns 1 and the frame when one complete frame is buffered, 0 when more
 * bytes are needed, and -1 for a frame this protocol cannot carry. */
static int link_reader_take(h2_lua_link_reader_t *reader, uint8_t *out_type,
                            uint8_t *out_payload, size_t *out_len) {
  size_t len;
  if (reader->len < H2_LUA_LINK_FRAME_HEADER) {
    return 0;
  }
  len = ((size_t)reader->data[1] << 8u) | reader->data[2];
  if (len > H2_LUA_LINK_STREAM_CHUNK ||
      reader->data[0] < H2_LUA_LINK_FRAME_HELLO ||
      reader->data[0] > H2_LUA_LINK_FRAME_STREAM) {
    return -1;
  }
  if (reader->len < H2_LUA_LINK_FRAME_HEADER + len) {
    return 0;
  }
  *out_type = reader->data[0];
  memcpy(out_payload, reader->data + H2_LUA_LINK_FRAME_HEADER, len);
  *out_len = len;
  reader->len -= H2_LUA_LINK_FRAME_HEADER + len;
  memmove(reader->data, reader->data + H2_LUA_LINK_FRAME_HEADER + len,
          reader->len);
  return 1;
}

static void link_send_bye(h2_bleikcp_t *stream) {
  if (link_write_frame(stream, H2_LUA_LINK_FRAME_BYE, NULL, 0u, NULL, 0u,
                       H2_LUA_LINK_BYE_FLUSH_MS) == H2_PAL_OK) {
    (void)h2_bleikcp_flush(stream, H2_LUA_LINK_BYE_FLUSH_MS);
  }
}

/*
 * Runs the HELLO handshake and then the receive loop on one ready stream.
 * Owns the stream until it returns; the stream is published for send() only
 * between a validated HELLO and the return.
 */
static h2_lua_link_outcome_t link_run_stream(h2_lua_link_t *link,
                                             h2_bleikcp_t *stream,
                                             uint64_t setup_deadline_ms,
                                             int *out_result) {
  h2_lua_link_reader_t reader = {.len = 0u};
  uint8_t payload[H2_LUA_LINK_STREAM_CHUNK];
  const uint8_t version = H2_LUA_LINK_PROTOCOL_VERSION;
  h2_lua_link_outcome_t outcome = H2_LUA_LINK_OUTCOME_LOCAL;
  int connected = 0;
  int rc;
  *out_result = H2_PAL_OK;
  rc = link_write_frame(stream, H2_LUA_LINK_FRAME_HELLO, &version, 1u,
                        link->tag, link->tag_len, H2_LUA_LINK_SLICE_MS);
  if (rc != H2_PAL_OK) {
    *out_result = rc;
    return H2_LUA_LINK_OUTCOME_SETUP_FAILED;
  }
  for (;;) {
    size_t read_len = 0u;
    if (link_is_closing(link)) {
      link_send_bye(stream);
      outcome = H2_LUA_LINK_OUTCOME_LOCAL;
      break;
    }
    if (!connected && link_now_ms(link) >= setup_deadline_ms) {
      outcome = H2_LUA_LINK_OUTCOME_SETUP_TIMEOUT;
      break;
    }
    rc = h2_bleikcp_read(stream, reader.data + reader.len,
                         sizeof(reader.data) - reader.len, &read_len,
                         H2_LUA_LINK_SLICE_MS);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
      continue;
    }
    if (rc != H2_PAL_OK) {
      *out_result = rc;
      outcome = connected ? H2_LUA_LINK_OUTCOME_LOST
                          : H2_LUA_LINK_OUTCOME_SETUP_FAILED;
      break;
    }
    reader.len += read_len;
    for (;;) {
      uint8_t type = 0u;
      size_t len = 0u;
      int taken = link_reader_take(&reader, &type, payload, &len);
      if (taken == 0) {
        break;
      }
      if (taken < 0 || (connected && type == H2_LUA_LINK_FRAME_HELLO) ||
          (!connected && type != H2_LUA_LINK_FRAME_HELLO) ||
          (type == H2_LUA_LINK_FRAME_MESSAGE &&
           (len == 0u || len > H2_LUA_LINK_MESSAGE_MAX)) ||
          (type == H2_LUA_LINK_FRAME_STREAM && len == 0u)) {
        *out_result = H2_PAL_ERR_FORMAT;
        outcome = connected ? H2_LUA_LINK_OUTCOME_LOST
                            : H2_LUA_LINK_OUTCOME_MISMATCH;
        goto done;
      }
      if (type == H2_LUA_LINK_FRAME_HELLO) {
        if (len != 1u + link->tag_len || payload[0] != version ||
            memcmp(payload + 1u, link->tag, link->tag_len) != 0) {
          outcome = H2_LUA_LINK_OUTCOME_MISMATCH;
          goto done;
        }
        h2_bleikcp_stats_t stats = {0};
        (void)h2_bleikcp_get_stats(stream, &stats);
        link_lock(link);
        if (!link->closing) {
          link->stream = stream;
          link->state = H2_LUA_LINK_CONNECTED;
          link->max_datagram =
              stats.att_mtu > H2_PAL_BLE_ATT_HEADER_LEN
                  ? stats.att_mtu - H2_PAL_BLE_ATT_HEADER_LEN
                  : 0u;
          if (link->max_datagram > H2_LUA_LINK_UNRELIABLE_MAX) {
            link->max_datagram = H2_LUA_LINK_UNRELIABLE_MAX;
          }
          /* Nothing is queued before the handshake, so CONNECTED and the
           * early datagrams (at most 1 + 4 events) always fit. */
          (void)link_push_locked(link, H2_LUA_LINK_EVENT_CONNECTED);
          for (size_t i = 0u; i < link->early_datagram_count; ++i) {
            h2_lua_link_event_t *early =
                link_push_locked(link, H2_LUA_LINK_EVENT_MESSAGE);
            early->len = link->early_datagram_len[i];
            memcpy(early->data, link->early_datagrams[i], early->len);
          }
          link->early_datagram_count = 0u;
          h2_lua_host_wake_job(link->job);
        }
        link_unlock(link);
        connected = 1;
      } else if (type == H2_LUA_LINK_FRAME_BYE) {
        *out_result = H2_PAL_ERR_CLOSED;
        outcome = H2_LUA_LINK_OUTCOME_PEER_CLOSED;
        goto done;
      } else if ((type == H2_LUA_LINK_FRAME_MESSAGE
                      ? link_post(link, H2_LUA_LINK_EVENT_MESSAGE, NULL,
                                  H2_PAL_OK, payload, len)
                      : link_stream_input(link, payload, len)) != H2_PAL_OK) {
        link_send_bye(stream);
        outcome = H2_LUA_LINK_OUTCOME_LOCAL;
        goto done;
      }
    }
  }
done:
  link_lock(link);
  link->stream = NULL;
  link_unlock(link);
  return outcome;
}

static int link_server_handler(void *user, h2_bleikcp_t *stream,
                               uint16_t conn_handle) {
  h2_lua_link_t *link = user;
  h2_lua_link_outcome_t outcome;
  int result = H2_PAL_OK;
  link_lock(link);
  if (link->closing || link->handler_done || link->peer_attached) {
    link_unlock(link);
    return H2_PAL_ERR_CLOSED;
  }
  link->peer_attached = 1;
  link->conn_handle = conn_handle;
  link->datagram_handle = link->datagram_value_handle;
  link_broadcast(link);
  link_unlock(link);
  outcome = link_run_stream(
      link, stream, link_now_ms(link) + H2_LUA_LINK_HELLO_TIMEOUT_MS, &result);
  link_lock(link);
  link->handler_outcome = outcome;
  link->handler_result = result;
  link->handler_done = 1;
  link_broadcast(link);
  link_unlock(link);
  return H2_PAL_OK;
}

static h2_pal_result_t link_start_advertising(h2_lua_link_t *link,
                                              h2_pal_ble_adv_set_t **out_set) {
  const h2_pal_ble_host_api_t *ble = link->runtime->ble_host;
  const h2_pal_ble_uuid_t uuid = {link->adv_uuid, H2_LUA_LINK_UUID_LEN};
  const int extended = link->config.adv_type == H2_PAL_BLE_ADV_TYPE_EXTENDED;
  const h2_pal_ble_adv_params_t params = {
      .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
      .interval_min_ms = H2_LUA_LINK_ADV_INTERVAL_MIN_MS,
      .interval_max_ms = H2_LUA_LINK_ADV_INTERVAL_MAX_MS,
      .type = link->config.adv_type,
      .primary_phy = extended ? H2_PAL_BLE_PHY_1M : H2_PAL_BLE_PHY_UNKNOWN,
      .secondary_phy = extended ? H2_PAL_BLE_PHY_1M : H2_PAL_BLE_PHY_UNKNOWN,
      .sid = extended ? H2_LUA_LINK_ADV_SID : 0u,
  };
  const h2_pal_ble_adv_data_t data = {
      .service_uuids = &uuid,
      .service_uuid_count = 1u,
  };
  h2_pal_result_t rc = h2_pal_ble_adv_set_create(ble, &params, out_set);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = h2_pal_ble_adv_set_set_data(ble, *out_set, &data);
  if (rc == H2_PAL_OK) {
    rc = h2_pal_ble_adv_set_start(ble, *out_set);
  }
  if (rc != H2_PAL_OK) {
    (void)h2_pal_ble_adv_set_destroy(ble, *out_set);
    *out_set = NULL;
  }
  return rc;
}

static void link_stop_advertising(h2_lua_link_t *link,
                                  h2_pal_ble_adv_set_t **set) {
  if (*set != NULL) {
    (void)h2_pal_ble_adv_set_stop(link->runtime->ble_host, *set);
    (void)h2_pal_ble_adv_set_destroy(link->runtime->ble_host, *set);
    *set = NULL;
  }
}

static void link_run_host(h2_lua_link_t *link) {
  const h2_bleikcp_api_t api = link_bleikcp_api(link);
  const h2_bleikcp_config_t config = link_bleikcp_config(link);
  h2_bleikcp_server_t *server = NULL;
  h2_pal_ble_adv_set_t *adv_set = NULL;
  h2_lua_link_outcome_t outcome = H2_LUA_LINK_OUTCOME_LOCAL;
  int result = H2_PAL_OK;
  int rc = h2_bleikcp_server_open(&api, &config, link_server_handler, link,
                                  &server);
  /* The datagram characteristic stays borrowed until server close. */
  if (rc == H2_PAL_OK) {
    rc = link_start_advertising(link, &adv_set);
  }
  if (rc != H2_PAL_OK) {
    outcome = H2_LUA_LINK_OUTCOME_SETUP_FAILED;
    result = rc;
  } else {
    const uint64_t deadline_ms =
        link->timeout_ms == 0u ? 0u : link->started_ms + link->timeout_ms;
    link_lock(link);
    for (;;) {
      if (link->handler_done) {
        outcome = link->handler_outcome;
        result = link->handler_result;
        break;
      }
      if (link->closing) {
        break;
      }
      if (link->peer_attached && adv_set != NULL) {
        link_unlock(link);
        link_stop_advertising(link, &adv_set);
        link_lock(link);
        continue;
      }
      if (!link->peer_attached && deadline_ms != 0u &&
          link_now_ms(link) >= deadline_ms) {
        outcome = H2_LUA_LINK_OUTCOME_SETUP_TIMEOUT;
        break;
      }
      link_wait(link, H2_LUA_LINK_SLICE_MS);
    }
    /* A local close lets an attached handler send BYE before the server
     * marks its borrowed stream closed. */
    if (link->closing && link->peer_attached) {
      const uint64_t exit_deadline_ms =
          link_now_ms(link) + H2_LUA_LINK_HANDLER_EXIT_MS;
      while (!link->handler_done && link_now_ms(link) < exit_deadline_ms) {
        link_wait(link, H2_LUA_LINK_SLICE_MS);
      }
    }
    link_unlock(link);
  }
  link_stop_advertising(link, &adv_set);
  if (server != NULL) {
    (void)h2_bleikcp_server_close(server);
  }
  link_post_outcome(link, outcome, result);
}

static bool link_scan_result(void *user,
                             const h2_pal_ble_scan_result_t *result) {
  h2_lua_link_t *link = user;
  int match = 0;
  if (result == NULL || !result->connectable ||
      result->data_status != H2_PAL_BLE_ADV_DATA_COMPLETE) {
    return false;
  }
  for (size_t i = 0u; i < result->service_uuid_count; ++i) {
    const h2_pal_ble_uuid_t *uuid = &result->service_uuids[i];
    if (uuid->data != NULL && uuid->len == H2_LUA_LINK_UUID_LEN &&
        memcmp(uuid->data, link->adv_uuid, H2_LUA_LINK_UUID_LEN) == 0) {
      match = 1;
      break;
    }
  }
  if (!match) {
    return false;
  }
  link_lock(link);
  if (!link->scan_found) {
    link->scan_found = 1;
    link->scan_addr = result->addr;
    link_broadcast(link);
  }
  link_unlock(link);
  return true;
}

static uint32_t link_remaining_ms(h2_lua_link_t *link, uint64_t deadline_ms) {
  const uint64_t now = link_now_ms(link);
  return now >= deadline_ms ? 0u : (uint32_t)(deadline_ms - now);
}

static int link_discover_one(h2_lua_link_t *link, uint16_t conn_handle,
                             h2_pal_ble_gatt_discovery_kind_t kind,
                             const uint8_t *uuid, size_t uuid_len,
                             uint16_t start_handle, uint16_t end_handle,
                             h2_pal_ble_gatt_discovery_entry_t *out) {
  h2_pal_ble_gatt_discovery_entry_t entries[8];
  size_t count = 0u;
  const h2_pal_ble_gatt_discovery_request_t request = {
      .kind = kind,
      .uuid_filter = {uuid, uuid_len},
      .start_handle = start_handle,
      .end_handle = end_handle,
  };
  int rc = h2_pal_ble_gatt_discover(link->runtime->ble_host, conn_handle,
                                    &request, entries, 8u, &count,
                                    H2_LUA_LINK_CONNECT_TIMEOUT_MS);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  for (size_t i = 0u; i < count; ++i) {
    if (entries[i].uuid.data != NULL && entries[i].uuid.len == uuid_len &&
        memcmp(entries[i].uuid.data, uuid, uuid_len) == 0) {
      *out = entries[i];
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

/* Finds the peer's datagram characteristic and enables its notifications. */
static int link_join_datagram(h2_lua_link_t *link, uint16_t conn_handle,
                              h2_pal_system_event_subscription_t **out_sub) {
  h2_pal_ble_gatt_discovery_entry_t service;
  h2_pal_ble_gatt_discovery_entry_t datagram;
  h2_pal_ble_gatt_discovery_entry_t cccd;
  int rc = link_discover_one(link, conn_handle,
                             H2_PAL_BLE_GATT_DISCOVERY_SERVICE,
                             s_service_uuid, sizeof(s_service_uuid), 1u,
                             UINT16_MAX, &service);
  if (rc == H2_PAL_OK) {
    rc = link_discover_one(link, conn_handle,
                           H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC,
                           s_datagram_uuid, sizeof(s_datagram_uuid),
                           service.start_handle, service.end_handle,
                           &datagram);
  }
  if (rc == H2_PAL_OK &&
      ((datagram.properties & H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP) == 0u ||
       (datagram.properties & H2_PAL_BLE_GATT_PROPERTY_NOTIFY) == 0u)) {
    rc = H2_PAL_ERR_UNSUPPORTED;
  }
  if (rc == H2_PAL_OK) {
    rc = link_discover_one(link, conn_handle,
                           H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR, s_cccd_uuid,
                           sizeof(s_cccd_uuid),
                           (uint16_t)(datagram.value_handle + 1u),
                           service.end_handle, &cccd);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_pal_system_event_subscribe(
        link->runtime->system_event,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION,
        link_datagram_client_event, link, out_sub);
  }
  if (rc == H2_PAL_OK) {
    const h2_pal_ble_gatt_subscribe_t subscribe = {
        .value_handle = datagram.value_handle,
        .cccd_handle = cccd.value_handle,
        .mode = H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY,
        .enable = true,
    };
    rc = h2_pal_ble_gatt_subscribe(link->runtime->ble_host, conn_handle,
                                   &subscribe,
                                   H2_LUA_LINK_CONNECT_TIMEOUT_MS);
  }
  if (rc == H2_PAL_OK) {
    link_lock(link);
    link->conn_handle = conn_handle;
    link->datagram_handle = datagram.value_handle;
    link_unlock(link);
  }
  return rc;
}

static void link_run_join(h2_lua_link_t *link) {
  const h2_pal_ble_host_api_t *ble = link->runtime->ble_host;
  const h2_bleikcp_api_t api = link_bleikcp_api(link);
  const h2_bleikcp_config_t config = link_bleikcp_config(link);
  const uint64_t deadline_ms = link->started_ms + link->timeout_ms;
  const h2_pal_ble_scan_params_t scan = {
      .mode = H2_PAL_BLE_SCAN_MODE_PASSIVE,
      .interval_ms = H2_LUA_LINK_SCAN_INTERVAL_MS,
      .window_ms = H2_LUA_LINK_SCAN_INTERVAL_MS,
      .type = link->config.scan_type,
      .phy_mask = H2_PAL_BLE_SCAN_PHY_1M,
  };
  h2_pal_ble_addr_t addr;
  uint16_t conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
  uint16_t mtu = 0u;
  h2_bleikcp_t *stream = NULL;
  h2_pal_system_event_subscription_t *datagram_sub = NULL;
  h2_lua_link_outcome_t outcome = H2_LUA_LINK_OUTCOME_LOCAL;
  int result = H2_PAL_OK;
  int found;
  int rc = h2_pal_ble_start_scan(ble, &scan, link_scan_result, link);
  if (rc != H2_PAL_OK) {
    link_post_outcome(link, H2_LUA_LINK_OUTCOME_SETUP_FAILED, rc);
    return;
  }
  link_lock(link);
  while (!link->scan_found && !link->closing &&
         link_now_ms(link) < deadline_ms) {
    link_wait(link, H2_LUA_LINK_SLICE_MS);
  }
  found = link->scan_found;
  addr = link->scan_addr;
  link_unlock(link);
  rc = h2_pal_ble_stop_scan(ble);
  (void)rc;
  if (link_is_closing(link)) {
    return;
  }
  if (!found) {
    (void)link_post(link, H2_LUA_LINK_EVENT_ERROR, "not_found",
                    H2_PAL_ERR_NOT_FOUND, NULL, 0u);
    return;
  }
  {
    uint32_t remaining = link_remaining_ms(link, deadline_ms);
    const h2_pal_ble_connect_params_t params = {
        .timeout_ms = remaining < H2_LUA_LINK_CONNECT_TIMEOUT_MS
                          ? remaining
                          : H2_LUA_LINK_CONNECT_TIMEOUT_MS,
        .interval_min_ms = H2_LUA_LINK_CONN_INTERVAL_MS,
        .interval_max_ms = H2_LUA_LINK_CONN_INTERVAL_MS,
        .latency = 0u,
        .supervision_timeout_ms = H2_LUA_LINK_SUPERVISION_TIMEOUT_MS,
    };
    if (remaining == 0u) {
      link_post_outcome(link, H2_LUA_LINK_OUTCOME_SETUP_TIMEOUT, 0);
      return;
    }
    rc = h2_pal_ble_connect(ble, &addr, &params, &conn_handle);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_pal_ble_exchange_mtu(ble, conn_handle, &mtu,
                                 H2_LUA_LINK_CONNECT_TIMEOUT_MS);
  }
  if (rc == H2_PAL_OK) {
    rc = h2_bleikcp_client_open(&api, &config, conn_handle, mtu, &stream);
  }
  if (rc == H2_PAL_OK) {
    rc = link_join_datagram(link, conn_handle, &datagram_sub);
  }
  if (rc != H2_PAL_OK) {
    outcome = H2_LUA_LINK_OUTCOME_SETUP_FAILED;
    result = rc;
  } else {
    uint64_t hello_deadline_ms = link_now_ms(link) + H2_LUA_LINK_HELLO_TIMEOUT_MS;
    if (hello_deadline_ms > deadline_ms) {
      hello_deadline_ms = deadline_ms;
    }
    outcome = link_run_stream(link, stream, hello_deadline_ms, &result);
  }
  if (datagram_sub != NULL) {
    h2_pal_system_event_unsubscribe(link->runtime->system_event,
                                    datagram_sub);
  }
  if (stream != NULL) {
    (void)h2_bleikcp_close(stream);
  }
  if (conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE) {
    (void)h2_pal_ble_disconnect(ble, conn_handle);
  }
  link_post_outcome(link, outcome, result);
}

static void link_session_entry(void *context) {
  h2_lua_link_t *link = context;
  if (link->role == H2_LUA_LINK_ROLE_HOST) {
    link_run_host(link);
  } else {
    link_run_join(link);
  }
  link_lock(link);
  link->stream = NULL;
  link->state = H2_LUA_LINK_IDLE;
  link->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
  link->datagram_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
  link->task_done = 1;
  link_broadcast(link);
  link_unlock(link);
}

/* Joins a finished session task so the slot can be reused. Called with the
 * link mutex held; the task no longer takes it once task_done is set. */
static int link_reclaim_locked(h2_lua_link_t *link) {
  if (!link->in_use) {
    return 1;
  }
  /* After the final event the task only publishes task_done, so an app that
   * reacts to LINK_DISCONNECTED/ERROR by hosting again is not told busy. */
  for (int i = 0; link->ended && !link->task_done && i < 20; ++i) {
    link_wait(link, H2_LUA_LINK_SLICE_MS);
  }
  if (!link->task_done) {
    return 0;
  }
  if (h2_pal_task_join(link->runtime->task, link->task) != H2_PAL_OK) {
    return 0;
  }
  link->task = NULL;
  link->in_use = 0;
  return 1;
}

static h2_lua_link_t *link_from_upvalue(lua_State *state) {
  return lua_touserdata(state, lua_upvalueindex(2));
}

static h2_lua_job_t *job_from_upvalue(lua_State *state) {
  return lua_touserdata(state, lua_upvalueindex(1));
}

static int link_start(lua_State *state, h2_lua_link_role_t role) {
  h2_lua_job_t *job = job_from_upvalue(state);
  h2_lua_link_t *link = link_from_upvalue(state);
  const char *tag;
  size_t tag_len = 0u;
  lua_Integer timeout_ms;
  h2_pal_result_t rc;
  luaL_checktype(state, 1, LUA_TTABLE);
  lua_getfield(state, 1, "tag");
  tag = lua_tolstring(state, -1, &tag_len);
  if (lua_type(state, -1) != LUA_TSTRING || tag_len == 0u ||
      tag_len > H2_LUA_LINK_TAG_MAX) {
    return luaL_argerror(state, 1, "tag must be a string of 1..32 bytes");
  }
  lua_getfield(state, 1, "timeout_ms");
  if (lua_isnil(state, -1)) {
    timeout_ms =
        role == H2_LUA_LINK_ROLE_HOST ? 0 : H2_LUA_LINK_JOIN_TIMEOUT_DEFAULT_MS;
  } else if (!lua_isinteger(state, -1)) {
    return luaL_argerror(state, 1, "timeout_ms must be an integer");
  } else {
    timeout_ms = lua_tointeger(state, -1);
  }
  if (timeout_ms < 0 || timeout_ms > H2_LUA_LINK_JOIN_TIMEOUT_MAX_MS ||
      (role == H2_LUA_LINK_ROLE_JOIN && timeout_ms == 0)) {
    return luaL_argerror(state, 1, "timeout_ms is out of range");
  }
  link_lock(link);
  if (!link_reclaim_locked(link)) {
    link_unlock(link);
    lua_pushnil(state);
    lua_pushliteral(state, "link: busy");
    return 2;
  }
  link->job = job;
  link->job_id = job->id;
  link->job_generation = job->generation;
  link->role = role;
  link->state = role == H2_LUA_LINK_ROLE_HOST ? H2_LUA_LINK_HOSTING
                                              : H2_LUA_LINK_JOINING;
  link->closing = 0;
  link->task_done = 0;
  link->ended = 0;
  link->timeout_ms = (uint32_t)timeout_ms;
  link->started_ms = link_now_ms(link);
  memcpy(link->tag, tag, tag_len);
  link->tag_len = tag_len;
  link_make_adv_uuid(link->tag, tag_len, link->adv_uuid);
  link->stream = NULL;
  link->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
  link->datagram_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
  link->max_datagram = 0u;
  link->datagram_value_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
  link->datagram_cccd_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
  link->datagram_characteristic = (h2_pal_ble_gatt_characteristic_t){
      .uuid = {s_datagram_uuid, sizeof(s_datagram_uuid)},
      .properties = H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP |
                    H2_PAL_BLE_GATT_PROPERTY_NOTIFY,
      .permissions = H2_PAL_BLE_GATT_PERMISSION_WRITE,
      .max_value_len = H2_LUA_LINK_UNRELIABLE_MAX,
      .write = link_datagram_server_write,
      .user = link,
      .out_value_handle = &link->datagram_value_handle,
      .out_cccd_handle = &link->datagram_cccd_handle,
  };
  link->peer_attached = 0;
  link->handler_done = 0;
  link->handler_outcome = H2_LUA_LINK_OUTCOME_LOCAL;
  link->handler_result = H2_PAL_OK;
  link->scan_found = 0;
  memset(&link->scan_addr, 0, sizeof(link->scan_addr));
  link_reset_buffers_locked(link);
  rc = h2_pal_task_start(link->runtime->task,
                         &(h2_pal_task_options_t){
                             .name = h2_lua_link_session_task_name,
                             .min_stack_size = H2_LUA_LINK_TASK_STACK_SIZE,
                         },
                         link_session_entry, link, &link->task);
  if (rc != H2_PAL_OK) {
    link->state = H2_LUA_LINK_IDLE;
    link->task = NULL;
    link_unlock(link);
    lua_pushnil(state);
    lua_pushfstring(state, "link: start failed: %d", (int)rc);
    return 2;
  }
  link->in_use = 1;
  link_unlock(link);
  lua_pushboolean(state, 1);
  return 1;
}

static int lua_link_host(lua_State *state) {
  return link_start(state, H2_LUA_LINK_ROLE_HOST);
}

static int lua_link_join(lua_State *state) {
  return link_start(state, H2_LUA_LINK_ROLE_JOIN);
}

static int link_connected_for(const h2_lua_link_t *link,
                              const h2_lua_job_t *job) {
  return link_session_owned_by(link, job->id, job->generation) &&
         !link->closing && link->state == H2_LUA_LINK_CONNECTED &&
         link->stream != NULL;
}

static int lua_link_send(lua_State *state) {
  h2_lua_job_t *job = job_from_upvalue(state);
  h2_lua_link_t *link = link_from_upvalue(state);
  size_t len = 0u;
  const char *data = luaL_checklstring(state, 1, &len);
  int rc;
  if (len == 0u || len > H2_LUA_LINK_MESSAGE_MAX) {
    return luaL_argerror(state, 1, "message must be 1..256 bytes");
  }
  link_lock(link);
  if (!link_connected_for(link, job)) {
    link_unlock(link);
    lua_pushnil(state);
    lua_pushliteral(state, "link: not connected");
    return 2;
  }
  rc = link_write_frame(link->stream, H2_LUA_LINK_FRAME_MESSAGE, NULL, 0u,
                        (const uint8_t *)data, len, 0u);
  link_unlock(link);
  if (rc == H2_PAL_OK) {
    lua_pushboolean(state, 1);
    return 1;
  }
  lua_pushnil(state);
  if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) {
    lua_pushliteral(state, "link: busy");
  } else {
    lua_pushliteral(state, "link: closed");
  }
  return 2;
}

static int lua_link_send_unreliable(lua_State *state) {
  h2_lua_job_t *job = job_from_upvalue(state);
  h2_lua_link_t *link = link_from_upvalue(state);
  size_t len = 0u;
  const char *data = luaL_checklstring(state, 1, &len);
  h2_lua_link_role_t role;
  uint16_t conn_handle;
  uint16_t attr_handle;
  size_t max_datagram;
  int rc;
  if (len == 0u || len > H2_LUA_LINK_UNRELIABLE_MAX) {
    return luaL_argerror(state, 1, "message must be 1..244 bytes");
  }
  link_lock(link);
  if (!link_connected_for(link, job)) {
    link_unlock(link);
    lua_pushnil(state);
    lua_pushliteral(state, "link: not connected");
    return 2;
  }
  role = link->role;
  conn_handle = link->conn_handle;
  attr_handle = link->datagram_handle;
  max_datagram = link->max_datagram;
  link_unlock(link);
  if (len > max_datagram) {
    lua_pushnil(state);
    lua_pushliteral(state, "link: too large");
    return 2;
  }
  /* Outside the link mutex: the Host may deliver synchronously. */
  if (role == H2_LUA_LINK_ROLE_HOST) {
    rc = h2_pal_ble_notify(link->runtime->ble_host, conn_handle, attr_handle,
                           (const uint8_t *)data, len);
  } else {
    rc = h2_pal_ble_gatt_write(link->runtime->ble_host, conn_handle,
                               attr_handle, (const uint8_t *)data, len, false,
                               0u);
  }
  if (rc == H2_PAL_OK) {
    lua_pushboolean(state, 1);
    return 1;
  }
  lua_pushnil(state);
  lua_pushliteral(state, "link: busy");
  return 2;
}

static int lua_link_write(lua_State *state) {
  h2_lua_job_t *job = job_from_upvalue(state);
  h2_lua_link_t *link = link_from_upvalue(state);
  size_t len = 0u;
  const uint8_t *data = (const uint8_t *)luaL_checklstring(state, 1, &len);
  size_t written = 0u;
  int rc = H2_PAL_OK;
  link_lock(link);
  if (!link_connected_for(link, job)) {
    link_unlock(link);
    lua_pushnil(state);
    lua_pushliteral(state, "link: not connected");
    return 2;
  }
  while (written < len) {
    size_t chunk = len - written;
    if (chunk > H2_LUA_LINK_STREAM_CHUNK) {
      chunk = H2_LUA_LINK_STREAM_CHUNK;
    }
    rc = link_write_frame(link->stream, H2_LUA_LINK_FRAME_STREAM, NULL, 0u,
                          data + written, chunk, 0u);
    if (rc != H2_PAL_OK) {
      break;
    }
    written += chunk;
  }
  link_unlock(link);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK &&
      rc != H2_PAL_ERR_TIMEOUT) {
    lua_pushnil(state);
    lua_pushliteral(state, "link: closed");
    return 2;
  }
  lua_pushinteger(state, (lua_Integer)written);
  return 1;
}

/* Stack while waiting: [1] max bytes, [2] deadline in monotonic ms. */
static int link_read_step(lua_State *state);

static int link_read_continue(lua_State *state, int status,
                              lua_KContext context) {
  (void)status;
  (void)context;
  return link_read_step(state);
}

static int link_read_step(lua_State *state) {
  h2_lua_job_t *job = job_from_upvalue(state);
  h2_lua_link_t *link = link_from_upvalue(state);
  uint8_t out[H2_LUA_LINK_STREAM_BUFFER_SIZE];
  const size_t max = (size_t)lua_tointeger(state, 1);
  const uint64_t deadline_ms = (uint64_t)lua_tointeger(state, 2);
  size_t len = 0u;
  int connected;
  int ended;
  uint64_t now;
  h2_lua_task_t *task;
  link_lock(link);
  if (!link_session_owned_by(link, job->id, job->generation)) {
    link_unlock(link);
    lua_pushnil(state);
    lua_pushliteral(state, "link: not connected");
    return 2;
  }
  /* Bytes received before a disconnect stay readable until drained. */
  len = link->stream_rx_len < max ? link->stream_rx_len : max;
  for (size_t i = 0u; i < len; ++i) {
    out[i] = link->stream_rx[(link->stream_rx_head + i) %
                             H2_LUA_LINK_STREAM_BUFFER_SIZE];
  }
  link->stream_rx_head =
      (link->stream_rx_head + len) % H2_LUA_LINK_STREAM_BUFFER_SIZE;
  link->stream_rx_len -= len;
  connected = link_connected_for(link, job);
  ended = link->closing || link->task_done;
  if (len != 0u) {
    link_broadcast(link);
  }
  link_unlock(link);
  if (len != 0u) {
    lua_pushlstring(state, (const char *)out, len);
    return 1;
  }
  if (!connected) {
    lua_pushnil(state);
    if (ended) {
      lua_pushliteral(state, "link: closed");
    } else {
      lua_pushliteral(state, "link: not connected");
    }
    return 2;
  }
  now = link_now_ms(link);
  if (now >= deadline_ms) {
    lua_pushliteral(state, "");
    return 1;
  }
  task = h2_lua_current_task(state);
  if (task == NULL) {
    return luaL_error(state, "link.read must run in a scheduler task");
  }
  task->wake_ms = now + H2_LUA_LINK_READ_POLL_MS < deadline_ms
                      ? now + H2_LUA_LINK_READ_POLL_MS
                      : deadline_ms;
  task->state = H2_LUA_TASK_SLEEPING;
  return lua_yieldk(state, 0, 0, link_read_continue);
}

static int lua_link_read(lua_State *state) {
  h2_lua_link_t *link = link_from_upvalue(state);
  lua_Integer max = luaL_optinteger(state, 1, H2_LUA_LINK_STREAM_BUFFER_SIZE);
  lua_Integer timeout_ms = luaL_optinteger(state, 2, 0);
  if (max < 1 || max > H2_LUA_LINK_STREAM_BUFFER_SIZE) {
    return luaL_argerror(state, 1, "max must be 1..4096 bytes");
  }
  if (timeout_ms < 0 || timeout_ms > (lua_Integer)UINT32_MAX) {
    return luaL_argerror(state, 2, "timeout_ms is out of range");
  }
  lua_settop(state, 0);
  lua_pushinteger(state, max);
  lua_pushinteger(state,
                  (lua_Integer)(link_now_ms(link) + (uint64_t)timeout_ms));
  return link_read_step(state);
}

static int lua_link_close(lua_State *state) {
  h2_lua_job_t *job = job_from_upvalue(state);
  h2_lua_link_t *link = link_from_upvalue(state);
  link_lock(link);
  if (link_session_owned_by(link, job->id, job->generation)) {
    link->closing = 1;
    link_reset_buffers_locked(link);
    link_broadcast(link);
  }
  link_unlock(link);
  lua_pushboolean(state, 1);
  return 1;
}

static int lua_link_state(lua_State *state) {
  static const char *const names[] = {"idle", "hosting", "joining",
                                      "connected"};
  h2_lua_job_t *job = job_from_upvalue(state);
  h2_lua_link_t *link = link_from_upvalue(state);
  h2_lua_link_state_t value = H2_LUA_LINK_IDLE;
  link_lock(link);
  if (link_session_owned_by(link, job->id, job->generation) &&
      !link->closing && !link->task_done) {
    value = link->state;
  }
  link_unlock(link);
  lua_pushstring(state, names[value]);
  return 1;
}

static void link_set_function(lua_State *state, const char *name,
                              lua_CFunction function, h2_lua_job_t *job,
                              h2_lua_link_t *link) {
  lua_pushlightuserdata(state, job);
  lua_pushlightuserdata(state, link);
  lua_pushcclosure(state, function, 2);
  lua_setfield(state, -2, name);
}

static void link_open_module(void *user, lua_State *state, h2_lua_job_t *job) {
  h2_lua_link_t *link = user;
  link_set_function(state, "host", lua_link_host, job, link);
  link_set_function(state, "join", lua_link_join, job, link);
  link_set_function(state, "send", lua_link_send, job, link);
  link_set_function(state, "send_unreliable", lua_link_send_unreliable, job,
                    link);
  link_set_function(state, "write", lua_link_write, job, link);
  link_set_function(state, "read", lua_link_read, job, link);
  link_set_function(state, "close", lua_link_close, job, link);
  link_set_function(state, "state", lua_link_state, job, link);
}

static void link_push_event(lua_State *state,
                            const h2_lua_link_event_t *event) {
  lua_createtable(state, 0, 8);
  lua_pushinteger(state, 0);
  lua_setfield(state, -2, "component_id");
  lua_pushinteger(state, 0);
  lua_setfield(state, -2, "component_kind");
  lua_pushinteger(state, (lua_Integer)event->kind);
  lua_setfield(state, -2, "event_type");
  lua_pushinteger(state, (lua_Integer)event->sequence);
  lua_setfield(state, -2, "sequence");
  lua_pushinteger(state, (lua_Integer)event->timestamp_ms);
  lua_setfield(state, -2, "timestamp_ms");
  switch (event->kind) {
  case H2_LUA_LINK_EVENT_CONNECTED:
    lua_pushstring(state,
                   event->role == H2_LUA_LINK_ROLE_HOST ? "host" : "join");
    lua_setfield(state, -2, "role");
    lua_pushinteger(state, (lua_Integer)event->max_datagram);
    lua_setfield(state, -2, "max_datagram");
    break;
  case H2_LUA_LINK_EVENT_MESSAGE:
    lua_pushlstring(state, (const char *)event->data, event->len);
    lua_setfield(state, -2, "data");
    lua_pushboolean(state, event->reliable);
    lua_setfield(state, -2, "reliable");
    break;
  default:
    lua_pushstring(state, event->reason == NULL ? "" : event->reason);
    lua_setfield(state, -2, "reason");
    lua_pushinteger(state, event->result);
    lua_setfield(state, -2, "result");
    break;
  }
}

static int link_callback_in_flight(const h2_lua_job_t *job) {
  for (size_t i = 0u; i < job->host->config.max_coroutines_per_vm; ++i) {
    const h2_lua_task_t *task = &job->tasks[i];
    if (task->ordered_callback &&
        (task->state == H2_LUA_TASK_READY ||
         task->state == H2_LUA_TASK_SLEEPING ||
         task->state == H2_LUA_TASK_JOINING ||
         task->state == H2_LUA_TASK_CAPABILITY)) {
      return 1;
    }
  }
  return 0;
}

/* Runs on the owning worker with the job mutex held. Scheduler tasks do not
 * run in spawn order, so one event's callbacks must finish before the next
 * event is dispatched; this keeps LINK_MESSAGE in arrival order. */
static void link_deliver(void *user, h2_lua_job_t *job) {
  h2_lua_link_t *link = user;
  int popped = 0;
  link_lock(link);
  if (!link_session_owned_by(link, job->id, job->generation)) {
    link_unlock(link);
    return;
  }
  while (link->event_count != 0u && !link_callback_in_flight(job)) {
    const h2_lua_link_event_t *event = &link->events[link->event_head];
    for (size_t i = link->next_callback_index; i < job->callback_count; ++i) {
      h2_lua_callback_t *callback = &job->callbacks[i];
      uint32_t task_id = 0u;
      if (!callback->active ||
          callback->component_id != H2_RUNTIME_COMPONENT_ID_NONE ||
          callback->kind != event->kind) {
        continue;
      }
      lua_rawgeti(job->vm->state, LUA_REGISTRYINDEX, callback->lua_ref);
      link_push_event(job->vm->state, event);
      if (h2_lua_spawn_task(job, job->vm->state,
                            lua_absindex(job->vm->state, -2), 1,
                            &task_id) != H2_PAL_OK) {
        lua_pop(job->vm->state, 2);
        link->next_callback_index = i;
        goto out;
      }
      h2_lua_task_t *task = h2_lua_find_task(job, task_id);
      task->release_when_done = 1;
      task->ordered_callback = 1;
      lua_pop(job->vm->state, 2);
      link->next_callback_index = i + 1u;
    }
    link->next_callback_index = 0u;
    link->event_head = (link->event_head + 1u) % H2_LUA_LINK_EVENT_CAPACITY;
    link->event_count--;
    popped = 1;
  }
out:
  if (popped) {
    link_broadcast(link);
  }
  link_unlock(link);
}

static void link_job_ended(void *user, h2_lua_job_id_t job_id,
                           uint32_t job_generation) {
  h2_lua_link_t *link = user;
  link_lock(link);
  if (link_session_owned_by(link, job_id, job_generation)) {
    link->closing = 1;
    link_reset_buffers_locked(link);
    link_broadcast(link);
  }
  link_unlock(link);
}

static void link_destroy(void *user) {
  h2_lua_link_t *link = user;
  h2_pal_task_t *task;
  link_lock(link);
  link->closing = 1;
  link_broadcast(link);
  task = link->in_use ? link->task : NULL;
  link_unlock(link);
  if (task != NULL) {
    (void)h2_pal_task_join(link->runtime->task, task);
  }
  (void)h2_pal_cond_destroy(link->runtime->sync, link->cond);
  (void)h2_pal_mutex_destroy(link->runtime->sync, link->mutex);
  h2_pal_mem_free(link->runtime->mem, link);
}

static const h2_lua_link_hooks_t s_link_hooks = {
    .open_module = link_open_module,
    .deliver = link_deliver,
    .job_ended = link_job_ended,
    .destroy = link_destroy,
};

/* A board without BLE wires the canonical unsupported object, whose
 * operations exist but always fail; treat it like a missing BLE Host. */
static int link_ble_is_usable(const h2_pal_ble_host_api_t *ble) {
  const h2_pal_ble_vtable_t *v = ble == NULL ? NULL : ble->vtable;
  return v != NULL && v != h2_pal_unsupported_ble_host_api()->vtable &&
         v->adv_set_create != NULL &&
         v->adv_set_set_data != NULL && v->adv_set_start != NULL &&
         v->adv_set_stop != NULL && v->adv_set_destroy != NULL &&
         v->start_scan != NULL && v->stop_scan != NULL &&
         v->register_gatt_services != NULL &&
         v->unregister_gatt_services != NULL && v->notify != NULL &&
         v->connect != NULL && v->disconnect != NULL &&
         v->exchange_mtu != NULL && v->gatt_discover != NULL &&
         v->gatt_write != NULL && v->gatt_subscribe != NULL;
}

static int link_system_event_is_usable(const h2_pal_system_event_api_t *api) {
  return api != NULL && api->vtable != NULL &&
         api->vtable != h2_pal_unsupported_system_event_api()->vtable &&
         api->vtable->subscribe != NULL && api->vtable->unsubscribe != NULL;
}

h2_pal_result_t h2_lua_link_enable(h2_lua_host_t *host,
                                   const h2_lua_link_config_t *config) {
  h2_runtime_t *runtime;
  h2_lua_link_t *link;
  h2_pal_result_t rc;
  if (host == NULL || config == NULL ||
      (config->adv_type != H2_PAL_BLE_ADV_TYPE_LEGACY &&
       config->adv_type != H2_PAL_BLE_ADV_TYPE_EXTENDED) ||
      (config->scan_type != H2_PAL_BLE_SCAN_TYPE_LEGACY &&
       config->scan_type != H2_PAL_BLE_SCAN_TYPE_EXTENDED)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (atomic_load(&host->started) != 0 || host->link_hooks != NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  runtime = host->config.runtime;
  if (!link_ble_is_usable(runtime->ble_host) ||
      !link_system_event_is_usable(runtime->system_event) ||
      runtime->sync == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  link = h2_pal_mem_alloc(runtime->mem, sizeof(*link));
  if (link == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(link, 0, sizeof(*link));
  link->runtime = runtime;
  link->config = *config;
  rc = h2_pal_mutex_create(runtime->sync,
                           &(h2_pal_mutex_config_t){
                               .name = h2_lua_link_session_task_name,
                               .allocator = runtime->mem,
                           },
                           &link->mutex);
  if (rc == H2_PAL_OK) {
    rc = h2_pal_cond_create(runtime->sync,
                            &(h2_pal_cond_config_t){
                                .name = h2_lua_link_session_task_name,
                                .allocator = runtime->mem,
                            },
                            &link->cond);
  }
  if (rc != H2_PAL_OK) {
    if (link->mutex != NULL) {
      (void)h2_pal_mutex_destroy(runtime->sync, link->mutex);
    }
    h2_pal_mem_free(runtime->mem, link);
    return rc;
  }
  host->link_user = link;
  host->link_hooks = &s_link_hooks;
  return H2_PAL_OK;
}
