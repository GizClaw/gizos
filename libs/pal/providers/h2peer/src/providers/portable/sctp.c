#include "sctp.h"

#include "config.h"
#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define SCTP_MAX_DATA_CHANNEL_MESSAGE_SIZE (64u * 1024u)
#define SCTP_RECEIVE_BUFFER_SIZE (256u * 1024u)
#define SCTP_PACKET_MTU (CONFIG_MTU - 100u)
#define SCTP_STREAM_COUNT 300u
#define SCTP_PORT 5000u
#define SCTP_COOKIE_LIFETIME_MS 60000u
#define SCTP_DCEP_RELIABLE 0x00u
#define SCTP_DCEP_PARTIAL_REXMIT 0x01u
#define SCTP_DCEP_PARTIAL_TIMED 0x02u
#define SCTP_DCEP_UNORDERED 0x80u

_Static_assert(SCTP_PACKET_MTU >= H2_PAL_SCTP_MIN_PACKET_SIZE,
               "SCTP packet MTU must fit the PAL minimum");
_Static_assert(SCTP_RECEIVE_BUFFER_SIZE >= SCTP_MAX_DATA_CHANNEL_MESSAGE_SIZE,
               "SCTP receive buffer must fit one maximum message");

static SctpStreamEntry* sctp_find_stream(Sctp* sctp, uint16_t sid) {
  for (size_t i = 0u; i < sctp->stream_count; ++i) {
    if (sctp->stream_table[i].sid == sid) {
      return &sctp->stream_table[i];
    }
  }
  return NULL;
}

static int sctp_add_stream_mapping(Sctp* sctp, const char* label, uint16_t sid,
                                   bool unordered,
                                   h2_pal_sctp_reliability_t reliability,
                                   uint32_t reliability_value) {
  if (sctp == NULL || sctp->mem == NULL || label == NULL) {
    return -1;
  }
  size_t label_len = 0u;
  while (label_len < sizeof(((SctpStreamEntry*)0)->label) &&
         label[label_len] != '\0') {
    label_len++;
  }
  if (label_len == 0u || label_len >= sizeof(((SctpStreamEntry*)0)->label)) {
    return -1;
  }
  SctpStreamEntry* existing = sctp_find_stream(sctp, sid);
  if (existing != NULL) {
    return -1;
  }
  if (sctp->stream_count >= SCTP_STREAM_COUNT) {
    return -1;
  }
  if (sctp->stream_count == sctp->stream_capacity) {
    size_t capacity =
        sctp->stream_capacity == 0u ? 8u : sctp->stream_capacity * 2u;
    if (capacity > SCTP_STREAM_COUNT) {
      capacity = SCTP_STREAM_COUNT;
    }
    if (capacity < sctp->stream_capacity ||
        capacity > SIZE_MAX / sizeof(*sctp->stream_table)) {
      return -1;
    }
    SctpStreamEntry* table =
        h2_pal_mem_alloc(sctp->mem, capacity * sizeof(*sctp->stream_table));
    if (table == NULL) {
      return -1;
    }
    memset(table, 0, capacity * sizeof(*sctp->stream_table));
    if (sctp->stream_table != NULL) {
      memcpy(table, sctp->stream_table,
             sctp->stream_count * sizeof(*sctp->stream_table));
      h2_pal_mem_free(sctp->mem, sctp->stream_table);
    }
    sctp->stream_table = table;
    sctp->stream_capacity = capacity;
  }
  SctpStreamEntry* entry = &sctp->stream_table[sctp->stream_count++];
  memcpy(entry->label, label, label_len + 1u);
  entry->sid = sid;
  entry->unordered = unordered;
  entry->reliability = reliability;
  entry->reliability_value = reliability_value;
  return 0;
}

int sctp_register_data_channel(Sctp* sctp, const char* label, uint16_t sid,
                               uint8_t channel_type,
                               uint32_t reliability_parameter) {
  bool unordered = (channel_type & SCTP_DCEP_UNORDERED) != 0u;
  uint8_t reliability_type =
      (uint8_t)(channel_type & (uint8_t)~SCTP_DCEP_UNORDERED);
  h2_pal_sctp_reliability_t reliability;
  uint32_t reliability_value = reliability_parameter;
  if (reliability_type == SCTP_DCEP_RELIABLE) {
    reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE;
    reliability_value = 0u;
  } else if (reliability_type == SCTP_DCEP_PARTIAL_REXMIT) {
    reliability = H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS;
  } else if (reliability_type == SCTP_DCEP_PARTIAL_TIMED) {
    reliability = H2_PAL_SCTP_RELIABILITY_MAX_LIFETIME_MS;
  } else {
    return -1;
  }
  return sctp_add_stream_mapping(sctp, label, sid, unordered, reliability,
                                 reliability_value);
}

int sctp_unregister_data_channel(Sctp* sctp, uint16_t sid) {
  if (sctp == NULL) {
    return -1;
  }
  for (size_t i = 0u; i < sctp->stream_count; ++i) {
    if (sctp->stream_table[i].sid != sid) {
      continue;
    }
    size_t remaining = sctp->stream_count - i - 1u;
    if (remaining != 0u) {
      memmove(&sctp->stream_table[i], &sctp->stream_table[i + 1u],
              remaining * sizeof(*sctp->stream_table));
    }
    sctp->stream_count--;
    memset(&sctp->stream_table[sctp->stream_count], 0,
           sizeof(*sctp->stream_table));
    return 0;
  }
  return -1;
}

static uint16_t sctp_read_be16(const void* data) {
  const uint8_t* bytes = (const uint8_t*)data;
  return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
}

static uint32_t sctp_read_be32(const void* data) {
  const uint8_t* bytes = (const uint8_t*)data;
  return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
         ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static bool sctp_dcep_ack_valid(const char* data, size_t len) {
  if (data == NULL || len == 0u || (uint8_t)data[0] != DATA_CHANNEL_ACK) {
    return false;
  }
  if (len == 1u) {
    return true;
  }
  return len == 4u && data[1] == 0 && data[2] == 0 && data[3] == 0;
}

static void sctp_reject_stream(Sctp* sctp, uint16_t sid) {
  if (sctp == NULL) {
    return;
  }
  (void)sctp_close_stream(sctp, sid);
}

static h2_pal_result_t sctp_now(Sctp* sctp, uint64_t* out_now_ms) {
  return h2_pal_time_get_monotonic_ms(sctp->time, out_now_ms);
}

static h2_pal_result_t sctp_emit_packet(void* user,
                                        h2_pal_sctp_association_t* association,
                                        const uint8_t* packet,
                                        size_t packet_len) {
  Sctp* sctp = (Sctp*)user;
  (void)association;
  return dtls_srtp_write(sctp->dtls_srtp, packet, packet_len);
}

static void sctp_on_state(void* user, h2_pal_sctp_association_t* association,
                          h2_pal_sctp_state_t state, h2_pal_result_t reason) {
  Sctp* sctp = (Sctp*)user;
  (void)association;
  if (state == H2_PAL_SCTP_STATE_CONNECTED && !sctp->connected) {
    sctp->connected = 1;
    sctp->open_pending = 1;
  } else if ((state == H2_PAL_SCTP_STATE_CLOSED ||
              state == H2_PAL_SCTP_STATE_FAILED) &&
             sctp->connected) {
    if (state == H2_PAL_SCTP_STATE_FAILED) {
      H2_PEER_LOGE(sctp->log, "SCTP terminal state %d reason %d", (int)state,
                   (int)reason);
    }
    sctp->connected = 0;
    if (sctp->onclose != NULL) {
      sctp->onclose(sctp->userdata);
    }
  }
}

static void sctp_dispatch_open(Sctp* sctp) {
  if (sctp->open_pending) {
    sctp->open_pending = 0;
    if (sctp->onopen != NULL) {
      sctp->onopen(sctp->userdata);
    }
  }
}

static h2_pal_result_t sctp_on_message(
    void* user, h2_pal_sctp_association_t* association,
    const h2_pal_sctp_received_message_t* message) {
  Sctp* sctp = (Sctp*)user;
  (void)association;
  if (message == NULL || message->len > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return (h2_pal_result_t)sctp_handle_incoming_data(
      sctp, (char*)message->data, message->len, message->ppid,
      message->stream_id, 0);
}

static void sctp_on_stream_reset(
    void* user, h2_pal_sctp_association_t* association,
    const h2_pal_sctp_stream_reset_event_t* event) {
  Sctp* sctp = (Sctp*)user;
  (void)association;
  if (event != NULL && sctp->onstreamreset != NULL) {
    sctp->onstreamreset(event, sctp->userdata);
  }
}

int sctp_outgoing_data(Sctp* sctp, char* buf, size_t len, SctpDataPpid ppid,
                       uint16_t sid) {
  if (sctp == NULL || sctp->association == NULL || !sctp->connected ||
      buf == NULL) {
    return -1;
  }
  uint64_t now_ms = 0u;
  if (sctp_now(sctp, &now_ms) != H2_PAL_OK) {
    return -1;
  }
  const uint8_t empty = 0u;
  const int is_empty = ppid == PPID_STRING_EMPTY || ppid == PPID_BINARY_EMPTY;
  const SctpStreamEntry* stream =
      ppid == PPID_CONTROL ? NULL : sctp_find_stream(sctp, sid);
  if (stream != NULL && !stream->negotiated) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  const h2_pal_sctp_message_t message = {
      .data = is_empty ? &empty : (const uint8_t*)buf,
      .len = is_empty ? 1u : len,
      .stream_id = sid,
      .ppid = (uint32_t)ppid,
      .unordered = stream != NULL && stream->unordered,
      .reliability = stream != NULL ? stream->reliability
                                    : H2_PAL_SCTP_RELIABILITY_RELIABLE,
      .reliability_value = stream != NULL ? stream->reliability_value : 0u,
  };
  h2_pal_result_t result = h2_pal_sctp_association_send_message(
      sctp->api, sctp->association, &message, now_ms);
  if (result == H2_PAL_OK) {
    return (int)len;
  }
  if (result != H2_PAL_ERR_WOULD_BLOCK) {
    H2_PEER_LOGE(sctp->log, "SCTP send failed %d sid %u len %u errno %d",
                 (int)result, (unsigned int)sid, (unsigned int)len, errno);
  }
  return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_ERR_WOULD_BLOCK : -1;
}

void sctp_parse_data_channel_open(Sctp* sctp, uint16_t sid, char* data,
                                  size_t length) {
  if (sctp == NULL || data == NULL || length < 12u ||
      (uint8_t)data[0] != DATA_CHANNEL_OPEN || sid >= SCTP_STREAM_COUNT ||
      (sid & 1u) != sctp->remote_stream_first ||
      sctp_find_stream(sctp, sid) != NULL) {
    sctp_reject_stream(sctp, sid);
    return;
  }
  uint16_t label_length = sctp_read_be16(data + 8);
  uint16_t protocol_length = sctp_read_be16(data + 10);
  if (label_length == 0u ||
      (size_t)label_length >= sizeof(((SctpStreamEntry*)0)->label) ||
      (size_t)label_length + (size_t)protocol_length != length - 12u) {
    sctp_reject_stream(sctp, sid);
    return;
  }
  char label[sizeof(((SctpStreamEntry*)0)->label)];
  memcpy(label, data + 12, label_length);
  label[label_length] = '\0';
  if (sctp_register_data_channel(sctp, label, sid, (uint8_t)data[1],
                                 sctp_read_be32(data + 4)) != 0) {
    sctp_reject_stream(sctp, sid);
    return;
  }
  SctpStreamEntry* stream = sctp_find_stream(sctp, sid);
  if (stream == NULL) {
    return;
  }
  stream->remote_open_pending = true;
}

static void sctp_flush_remote_channel_opens(Sctp* sctp) {
  if (sctp == NULL || sctp->association_call_active) {
    return;
  }
  size_t index = 0u;
  while (index < sctp->stream_count) {
    SctpStreamEntry* stream = &sctp->stream_table[index];
    if (!stream->remote_open_pending) {
      index++;
      continue;
    }
    char ack = DATA_CHANNEL_ACK;
    int send_result =
        sctp_outgoing_data(sctp, &ack, 1u, PPID_CONTROL, stream->sid);
    if (send_result == H2_PAL_ERR_WOULD_BLOCK) {
      index++;
      continue;
    }
    if (send_result != 1) {
      uint16_t sid = stream->sid;
      (void)sctp_unregister_data_channel(sctp, sid);
      sctp_reject_stream(sctp, sid);
      continue;
    }
    stream->remote_open_pending = false;
    stream->negotiated = true;
    const SctpRemoteChannel remote = {
        .label = stream->label,
        .label_len = strlen(stream->label),
        .sid = stream->sid,
        .unordered = stream->unordered,
        .reliability = stream->reliability,
        .reliability_value = stream->reliability_value,
    };
    if (sctp->onremotechannel == NULL ||
        sctp->onremotechannel(&remote, sctp->userdata) != 0) {
      uint16_t sid = stream->sid;
      (void)sctp_unregister_data_channel(sctp, sid);
      sctp_reject_stream(sctp, sid);
      continue;
    }
    index++;
  }
}

int sctp_handle_incoming_data(Sctp* sctp, char* data, size_t len, uint32_t ppid,
                              uint16_t sid, int flags) {
  (void)flags;
  if (sctp == NULL || (data == NULL && len != 0u)) {
    return -1;
  }
  if (ppid == DATA_CHANNEL_PPID_CONTROL) {
    if (sctp_dcep_ack_valid(data, len)) {
      SctpStreamEntry* stream = sctp_find_stream(sctp, sid);
      if (stream != NULL && !stream->negotiated) {
        stream->negotiated = true;
        if (sctp->onlocalchannelopen != NULL) {
          sctp->onlocalchannelopen(sid, sctp->userdata);
        }
      }
      return 0;
    }
    if (len >= 1u && (uint8_t)data[0] == DATA_CHANNEL_ACK) {
      sctp_reject_stream(sctp, sid);
      return 0;
    }
    sctp_parse_data_channel_open(sctp, sid, data, len);
    sctp_flush_remote_channel_opens(sctp);
    return 0;
  }
  int is_text =
      ppid == DATA_CHANNEL_PPID_DOMSTRING || ppid == PPID_STRING_EMPTY;
  if (ppid != DATA_CHANNEL_PPID_DOMSTRING && ppid != DATA_CHANNEL_PPID_BINARY &&
      ppid != PPID_STRING_EMPTY && ppid != PPID_BINARY_EMPTY) {
    return 0;
  }
  SctpStreamEntry* stream = sctp_find_stream(sctp, sid);
  if (stream == NULL || !stream->negotiated) {
    return 0;
  }
  if (sctp->onmessage != NULL) {
    size_t delivered_len =
        (ppid == PPID_STRING_EMPTY || ppid == PPID_BINARY_EMPTY) ? 0u : len;
    return sctp->onmessage(data, delivered_len, sctp->userdata, sid, is_text);
  }
  return 0;
}

void sctp_incoming_data(Sctp* sctp, char* buf, size_t len) {
  if (sctp == NULL || sctp->association == NULL || buf == NULL) {
    return;
  }
  uint64_t now_ms = 0u;
  if (sctp_now(sctp, &now_ms) != H2_PAL_OK) {
    return;
  }
  sctp->association_call_active = 1;
  h2_pal_result_t result = h2_pal_sctp_association_input_packet(
      sctp->api, sctp->association, (const uint8_t *)buf, len, now_ms);
  sctp->association_call_active = 0;
  if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
    H2_PEER_LOGE(sctp->log, "SCTP input failed %d len %u", (int)result,
                 (unsigned int)len);
  }
  sctp_flush_remote_channel_opens(sctp);
  sctp_dispatch_open(sctp);
}

int sctp_service(Sctp* sctp) {
  if (sctp == NULL || sctp->association == NULL) {
    return 0;
  }
  uint64_t now_ms = 0u;
  uint64_t next_deadline_ms = H2_PAL_SCTP_NO_DEADLINE;
  if (sctp_now(sctp, &now_ms) != H2_PAL_OK) {
    return -1;
  }
  sctp->association_call_active = 1;
  h2_pal_result_t result = h2_pal_sctp_association_service(
      sctp->api, sctp->association, now_ms, &next_deadline_ms);
  sctp->association_call_active = 0;
  sctp_flush_remote_channel_opens(sctp);
  sctp_dispatch_open(sctp);
  return result == H2_PAL_OK || result == H2_PAL_ERR_WOULD_BLOCK ? 0 : -1;
}

int sctp_create_association(Sctp* sctp, DtlsSrtp* dtls_srtp) {
  if (sctp == NULL || sctp->api == NULL || sctp->time == NULL ||
      dtls_srtp == NULL) {
    return -1;
  }
  sctp->dtls_srtp = dtls_srtp;
  sctp->remote_stream_first =
      dtls_srtp->role == DTLS_SRTP_ROLE_SERVER ? 0u : 1u;
  const h2_pal_sctp_association_config_t config = {
      .role = dtls_srtp->role == DTLS_SRTP_ROLE_SERVER
                  ? H2_PAL_SCTP_ROLE_PASSIVE
                  : H2_PAL_SCTP_ROLE_ACTIVE,
      .local_port = SCTP_PORT,
      .remote_port = SCTP_PORT,
      .inbound_streams = SCTP_STREAM_COUNT,
      .outbound_streams = SCTP_STREAM_COUNT,
      .max_packet_size = SCTP_PACKET_MTU,
      .max_message_size = SCTP_MAX_DATA_CHANNEL_MESSAGE_SIZE,
      .send_buffer_size = SCTP_MAX_DATA_CHANNEL_MESSAGE_SIZE + SCTP_PACKET_MTU,
      .receive_buffer_size = SCTP_RECEIVE_BUFFER_SIZE,
      .cookie_lifetime_ms = SCTP_COOKIE_LIFETIME_MS,
      .callbacks =
          {
              .user = sctp,
              .emit_packet = sctp_emit_packet,
              .on_state = sctp_on_state,
              .on_message = sctp_on_message,
              .on_stream_reset = sctp_on_stream_reset,
          },
  };
  if (h2_pal_sctp_association_create(sctp->api, &config, &sctp->association) !=
      H2_PAL_OK) {
    return -1;
  }
  uint64_t now_ms = 0u;
  if (sctp_now(sctp, &now_ms) != H2_PAL_OK ||
      h2_pal_sctp_association_start(sctp->api, sctp->association, now_ms) !=
          H2_PAL_OK) {
    (void)h2_pal_sctp_association_close(sctp->api, &sctp->association);
    return -1;
  }
  return 0;
}

void sctp_destroy_association(Sctp* sctp) {
  if (sctp == NULL) {
    return;
  }
  if (sctp->association != NULL) {
    uint64_t now_ms = 0u;
    if (sctp_now(sctp, &now_ms) == H2_PAL_OK) {
      (void)h2_pal_sctp_association_shutdown(sctp->api, sctp->association,
                                             now_ms);
    }
    (void)h2_pal_sctp_association_close(sctp->api, &sctp->association);
  }
  sctp->connected = 0;
  sctp->open_pending = 0;
  sctp->association_call_active = 0;
  if (sctp->stream_table != NULL) {
    h2_pal_mem_free(sctp->mem, sctp->stream_table);
    sctp->stream_table = NULL;
  }
  sctp->stream_count = 0u;
  sctp->stream_capacity = 0u;
}

h2_pal_result_t sctp_close_stream(Sctp* sctp, uint16_t sid) {
  if (sctp == NULL || sctp->association == NULL || !sctp->connected) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  uint64_t now_ms = 0u;
  h2_pal_result_t result = sctp_now(sctp, &now_ms);
  if (result != H2_PAL_OK) {
    return result;
  }
  return h2_pal_sctp_association_reset_stream(sctp->api, sctp->association, sid,
                                              now_ms);
}

int sctp_is_connected(Sctp* sctp) { return sctp != NULL && sctp->connected; }

h2_pal_result_t sctp_is_writable(Sctp* sctp, bool* out_writable) {
  if (out_writable == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_writable = false;
  if (sctp == NULL || sctp->association == NULL || !sctp->connected) {
    return H2_PAL_OK;
  }
  return h2_pal_sctp_association_is_writable(sctp->api, sctp->association,
                                              out_writable);
}

void sctp_onmessage(Sctp* sctp,
                    h2_pal_result_t (*onmessage)(char* msg, size_t len,
                                                 void* userdata, uint16_t sid,
                                                 int is_text)) {
  sctp->onmessage = onmessage;
}

void sctp_onopen(Sctp* sctp, void (*onopen)(void* userdata)) {
  sctp->onopen = onopen;
}

void sctp_onclose(Sctp* sctp, void (*onclose)(void* userdata)) {
  sctp->onclose = onclose;
}

void sctp_onstreamreset(
    Sctp* sctp,
    void (*onstreamreset)(const h2_pal_sctp_stream_reset_event_t* event,
                          void* userdata)) {
  sctp->onstreamreset = onstreamreset;
}

void sctp_onlocalchannelopen(
    Sctp* sctp, void (*onlocalchannelopen)(uint16_t sid, void* userdata)) {
  sctp->onlocalchannelopen = onlocalchannelopen;
}

void sctp_onremotechannel(
    Sctp* sctp,
    int (*onremotechannel)(const SctpRemoteChannel* channel, void* userdata)) {
  sctp->onremotechannel = onremotechannel;
}
