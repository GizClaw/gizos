#include "peer_connection.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "agent.h"
#include "config.h"
#include "dtls_srtp.h"
#include "ports.h"
#include "rtcp.h"
#include "rtp.h"
#include "sctp.h"
#include "sdp.h"

#define STATE_CHANGED(pc, curr_state)                                 \
  if (pc->oniceconnectionstatechange && pc->state != curr_state) {    \
    pc->oniceconnectionstatechange(curr_state, pc->config.user_data); \
    pc->state = curr_state;                                           \
  }

struct PeerConnection {
  PeerConfiguration config;
  PeerConnectionState state;
  Agent agent;
  DtlsSrtp dtls_srtp;
  Sctp sctp;

  char sdp[CONFIG_SDP_BUFFER_SIZE];

  void (*onicecandidate)(char* sdp, void* user_data);
  void (*oniceconnectionstatechange)(PeerConnectionState state,
                                     void* user_data);
  void (*on_connected)(void* userdata);
  void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss,
                                  void* user_data);

  uint8_t temp_buf[CONFIG_MTU];
  uint8_t agent_buf[CONFIG_MTU];
  int agent_ret;
  int b_local_description_created;

  RtpEncoder artp_encoder;
  RtpEncoder vrtp_encoder;
  RtpDecoder vrtp_decoder;
  RtpDecoder artp_decoder;
  uint8_t pending_rtp[CONFIG_MTU + 128];
  size_t pending_rtp_len;

  uint32_t remote_assrc;
  uint32_t remote_vssrc;
};

enum {
  H2_PEER_COMPLETED_RECEIVE_TIMEOUT_MS = 2,
};

int peer_connection_rtp_send_or_queue(
    uint8_t* pending_packet, size_t pending_capacity, size_t* pending_len,
    uint8_t* packet, size_t packet_len, PeerConnectionRtpProtectFn protect,
    void* protect_user, PeerConnectionRtpSendFn send, void* send_user) {
  if (pending_packet == NULL || pending_len == NULL || packet == NULL ||
      protect == NULL || send == NULL || packet_len > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (*pending_len != 0u) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  int protected_size = (int)packet_len;
  if (protect(protect_user, packet, &protected_size) != 0 ||
      protected_size < 0 || (size_t)protected_size > pending_capacity) {
    return H2_PAL_ERR_IO;
  }
  int result = send(send_user, packet, protected_size);
  if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT) {
    memcpy(pending_packet, packet, (size_t)protected_size);
    *pending_len = (size_t)protected_size;
    return protected_size;
  }
  return result;
}

h2_pal_result_t peer_connection_rtp_flush(
    uint8_t* pending_packet, size_t* pending_len, PeerConnectionRtpSendFn send,
    void* send_user) {
  if (pending_packet == NULL || pending_len == NULL || send == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (*pending_len == 0u) {
    return H2_PAL_OK;
  }
  if (*pending_len > INT_MAX) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  int result = send(send_user, pending_packet, (int)*pending_len);
  if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_TIMEOUT) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (result < 0) {
    return result == H2_PAL_ERR_CLOSED ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_IO;
  }
  *pending_len = 0u;
  return H2_PAL_OK;
}

static int peer_connection_rtp_protect(
    void* user, uint8_t* packet, int* packet_len) {
  return dtls_srtp_encrypt_rtp_packet((DtlsSrtp*)user, packet, packet_len);
}

static int peer_connection_rtp_send(
    void* user, const uint8_t* packet, int packet_len) {
  return agent_send((Agent*)user, packet, packet_len);
}

static int peer_connection_outgoing_rtp_packet(
    uint8_t* data, size_t size, void* user_data) {
  PeerConnection* pc = (PeerConnection*)user_data;
  return peer_connection_rtp_send_or_queue(
      pc->pending_rtp, sizeof(pc->pending_rtp), &pc->pending_rtp_len, data,
      size, peer_connection_rtp_protect, &pc->dtls_srtp,
      peer_connection_rtp_send, &pc->agent);
}

static h2_pal_result_t peer_connection_flush_pending_rtp(
    PeerConnection* pc) {
  return peer_connection_rtp_flush(pc->pending_rtp, &pc->pending_rtp_len,
                                   peer_connection_rtp_send, &pc->agent);
}

static int peer_connection_dtls_srtp_send(
    void* ctx, const uint8_t* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  return agent_send(&pc->agent, buf, len);
}

static void peer_connection_incoming_rtcp(
    PeerConnection* pc, uint8_t* buf, size_t len) {
  size_t pos = 0;

  while (pos < len) {
    RtcpHeader rtcp_header;
    if (rtcp_parse_header(buf + pos, len - pos, &rtcp_header) != 0) {
      return;
    }

    switch (rtcp_header.type) {
    case RTCP_RR:
      H2_PEER_LOGD(pc->config.log, "RTCP_PR");
      break;
    case RTCP_PSFB: {
      int fmt = rtcp_header.rc;
      H2_PEER_LOGD(pc->config.log, "RTCP_PSFB %d", fmt);
      // PLI and FIR
      if ((fmt == 1 || fmt == 4) && pc->config.on_request_keyframe) {
        pc->config.on_request_keyframe(pc->config.user_data);
      }
    }
      default:
        break;
      }

    pos += ((size_t)rtcp_header.length + 1u) * 4u;
  }
}

static int peer_connection_process_completed_packet(
    PeerConnection* pc, uint8_t* packet, int packet_len) {
  if (rtcp_probe(packet, packet_len)) {
    H2_PEER_LOGD(pc->config.log, "Got RTCP packet");
    dtls_srtp_decrypt_rtcp_packet(&pc->dtls_srtp, packet, &packet_len);
    peer_connection_incoming_rtcp(pc, packet, packet_len);
    return packet_len;
  }
  if (dtls_srtp_probe(packet)) {
    int ret = dtls_srtp_read(&pc->dtls_srtp, packet, (size_t)packet_len,
                             pc->temp_buf, sizeof(pc->temp_buf));
    H2_PEER_LOGD(pc->config.log, "Got DTLS data %d", ret);
    if (ret > 0) {
      sctp_incoming_data(&pc->sctp, (char*)pc->temp_buf, ret);
    } else if (ret < 0) {
      packet_len = ret;
    }
    return packet_len;
  }
  if (rtp_packet_validate(packet, packet_len)) {
    H2_PEER_LOGD(pc->config.log, "Got RTP packet");
    dtls_srtp_decrypt_rtp_packet(&pc->dtls_srtp, packet, &packet_len);
    (void)rtp_decoders_decode(
        &pc->artp_decoder, &pc->vrtp_decoder, packet, (size_t)packet_len);
    if (pc->artp_decoder.last_event == RTP_DECODE_EVENT_LOSS) {
      H2_PEER_LOGW(pc->config.log,
                   "Opus RTP loss missing=%u next_sequence=%u ssrc=%u",
                   (unsigned int)pc->artp_decoder.last_loss_count,
                   (unsigned int)pc->artp_decoder.next_sequence,
                   (unsigned int)pc->artp_decoder.ssrc);
    } else if (pc->artp_decoder.last_event ==
               RTP_DECODE_EVENT_REORDER_WAIT) {
      H2_PEER_LOGD(pc->config.log,
                   "Opus RTP reorder wait sequence=%u next_sequence=%u ssrc=%u",
                   (unsigned int)pc->artp_decoder.reorder_sequence,
                   (unsigned int)pc->artp_decoder.next_sequence,
                   (unsigned int)pc->artp_decoder.ssrc);
    } else if (pc->artp_decoder.last_event ==
               RTP_DECODE_EVENT_REORDER_RECOVERED) {
      H2_PEER_LOGI(pc->config.log,
                   "Opus RTP reorder recovered next_sequence=%u ssrc=%u",
                   (unsigned int)pc->artp_decoder.next_sequence,
                   (unsigned int)pc->artp_decoder.ssrc);
    } else if (pc->artp_decoder.last_event == RTP_DECODE_EVENT_LATE) {
      H2_PEER_LOGW(pc->config.log,
                   "Opus RTP late-or-duplicate next_sequence=%u ssrc=%u",
                   (unsigned int)pc->artp_decoder.next_sequence,
                   (unsigned int)pc->artp_decoder.ssrc);
    } else if (pc->artp_decoder.last_event == RTP_DECODE_EVENT_RESET) {
      H2_PEER_LOGI(pc->config.log,
                   "Opus RTP sequence reset next_sequence=%u ssrc=%u",
                   (unsigned int)pc->artp_decoder.next_sequence,
                   (unsigned int)pc->artp_decoder.ssrc);
    }
    return packet_len;
  }
  H2_PEER_LOGW(pc->config.log, "Unknown data");
  return packet_len;
}

const char* peer_connection_state_to_string(
    PeerConnectionState state) {
  switch (state) {
    case PEER_CONNECTION_NEW:
      return "new";
    case PEER_CONNECTION_CHECKING:
      return "checking";
    case PEER_CONNECTION_CONNECTED:
      return "connected";
    case PEER_CONNECTION_COMPLETED:
      return "completed";
    case PEER_CONNECTION_FAILED:
      return "failed";
    case PEER_CONNECTION_CLOSED:
      return "closed";
    case PEER_CONNECTION_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

PeerConnectionState peer_connection_get_state(
    PeerConnection* pc) {
  return pc->state;
}

h2_pal_result_t peer_connection_classify_dtls_handshake_result(
    int handshake_result, PeerConnectionState* out_terminal_state) {
  if (out_terminal_state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (handshake_result < 0) {
    *out_terminal_state = PEER_CONNECTION_FAILED;
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

h2_pal_result_t peer_connection_state_poll_result(
    PeerConnectionState state) {
  if (state == PEER_CONNECTION_FAILED) {
    return H2_PAL_ERR_IO;
  }
  if (state == PEER_CONNECTION_DISCONNECTED ||
      state == PEER_CONNECTION_CLOSED) {
    return H2_PAL_ERR_CLOSED;
  }
  return H2_PAL_OK;
}

void* peer_connection_get_sctp(
    PeerConnection* pc) {
  return &pc->sctp;
}

h2_pal_result_t peer_connection_sctp_is_writable(PeerConnection* pc,
                                                  bool* out_writable) {
  if (out_writable == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_writable = false;
  if (pc == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return sctp_is_writable(&pc->sctp, out_writable);
}

PeerConnection* peer_connection_create(
    PeerConfiguration* config) {
  if (config == NULL || config->log == NULL || config->mem == NULL ||
      config->net == NULL || config->time == NULL || config->crypto == NULL ||
      config->dtls == NULL || config->sctp == NULL) {
    return NULL;
  }
  PeerConnection *pc = h2_pal_mem_alloc(config->mem, sizeof(PeerConnection));
  if (!pc) {
    return NULL;
  }
  memset(pc, 0, sizeof(*pc));

  memcpy(&pc->config, config, sizeof(PeerConfiguration));

  pc->agent.log = config->log;

  pc->agent.net = config->net;
  pc->agent.time = config->time;
  pc->agent.crypto = config->crypto;
  if (agent_create(&pc->agent) != 0) {
    h2_pal_mem_free(config->mem, pc);
    return NULL;
  }

  memset(&pc->sctp, 0, sizeof(pc->sctp));
  pc->sctp.log = config->log;
  pc->sctp.mem = config->mem;
  pc->sctp.api = config->sctp;
  pc->sctp.time = config->time;
  pc->sctp.userdata = config->user_data;

  if (pc->config.audio_codec) {
    rtp_encoder_init(&pc->artp_encoder, pc->config.audio_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->artp_decoder, pc->config.audio_codec,
                     pc->config.onaudiotrack, pc->config.user_data);
  }

  if (pc->config.video_codec) {
    rtp_encoder_init(&pc->vrtp_encoder, pc->config.video_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->vrtp_decoder, pc->config.video_codec,
                     pc->config.onvideotrack, pc->config.user_data);
  }

  return pc;
}

void peer_connection_destroy(
    PeerConnection* pc) {
  if (pc) {
    sctp_destroy_association(&pc->sctp);
    dtls_srtp_deinit(&pc->dtls_srtp);
    agent_destroy(&pc->agent);
    h2_pal_mem_free(pc->config.mem, pc);
    pc = NULL;
  }
}

void peer_connection_close(
    PeerConnection* pc) {
  pc->state = PEER_CONNECTION_CLOSED;
}

int peer_connection_send_audio(
    PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    return -1;
  }
  h2_pal_result_t flush_result = peer_connection_flush_pending_rtp(pc);
  if (flush_result != H2_PAL_OK) {
    return flush_result;
  }
  return rtp_encoder_encode(&pc->artp_encoder, buf, len);
}

int peer_connection_send_video(
    PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    return -1;
  }
  h2_pal_result_t flush_result = peer_connection_flush_pending_rtp(pc);
  if (flush_result != H2_PAL_OK) {
    return flush_result;
  }
  return rtp_encoder_encode(&pc->vrtp_encoder, buf, len);
}

int peer_connection_datachannel_send(
    PeerConnection* pc, char* message, size_t len) {
  return peer_connection_datachannel_send_sid(pc, message, len, 0);
}

int peer_connection_datachannel_send_sid(
    PeerConnection* pc, char* message, size_t len, uint16_t sid) {
  if (!sctp_is_connected(&pc->sctp)) {
    H2_PEER_LOGE(pc->config.log, "sctp not connected");
    return -1;
  }
  if (pc->config.datachannel == DATA_CHANNEL_STRING)
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_STRING, sid);
  else
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_BINARY, sid);
}

h2_pal_result_t peer_connection_datachannel_close_sid(
    PeerConnection* pc, uint16_t sid) {
  return pc == NULL ? H2_PAL_ERR_INVALID_ARG
                    : sctp_close_stream(&pc->sctp, sid);
}

int peer_connection_datachannel_forget_sid(
    PeerConnection* pc, uint16_t sid) {
  return pc == NULL ? -1 : sctp_unregister_data_channel(&pc->sctp, sid);
}

int peer_connection_create_datachannel(
    PeerConnection* pc, DecpChannelType channel_type, uint16_t priority,
    uint32_t reliability_parameter, char* label, char* protocol) {
  return peer_connection_create_datachannel_sid(
      pc, channel_type, priority, reliability_parameter, label, protocol, 0);
}

int peer_connection_encode_datachannel_open(
    uint8_t* out, size_t out_cap, DecpChannelType channel_type,
    uint16_t priority, uint32_t reliability_parameter, const char* label,
    const char* protocol, size_t* out_len) {
  if (out == NULL || label == NULL || protocol == NULL || out_len == NULL) {
    return -1;
  }
  size_t label_len = strlen(label);
  size_t protocol_len = strlen(protocol);
  if (label_len > UINT16_MAX || protocol_len > UINT16_MAX ||
      label_len > SIZE_MAX - 12u - protocol_len) {
    return -1;
  }
  size_t message_len = 12u + label_len + protocol_len;
  if (message_len > out_cap) {
    return -1;
  }

  memset(out, 0, message_len);
  out[0] = DATA_CHANNEL_OPEN;
  out[1] = (uint8_t)channel_type;
  out[2] = (uint8_t)(priority >> 8);
  out[3] = (uint8_t)priority;
  out[4] = (uint8_t)(reliability_parameter >> 24);
  out[5] = (uint8_t)(reliability_parameter >> 16);
  out[6] = (uint8_t)(reliability_parameter >> 8);
  out[7] = (uint8_t)reliability_parameter;
  out[8] = (uint8_t)(label_len >> 8);
  out[9] = (uint8_t)label_len;
  out[10] = (uint8_t)(protocol_len >> 8);
  out[11] = (uint8_t)protocol_len;
  memcpy(out + 12, label, label_len);
  memcpy(out + 12 + label_len, protocol, protocol_len);
  *out_len = message_len;
  return 0;
}

int peer_connection_create_datachannel_sid(
    PeerConnection* pc, DecpChannelType channel_type, uint16_t priority,
    uint32_t reliability_parameter, char* label, char* protocol, uint16_t sid) {
  int rtrn = -1;

  if (!sctp_is_connected(&pc->sctp)) {
    H2_PEER_LOGE(pc->config.log, "sctp not connected");
    return rtrn;
  }
  if (sctp_register_data_channel(&pc->sctp, label, sid, (uint8_t)channel_type,
                                 reliability_parameter) != 0) {
    return rtrn;
  }

  //  0                   1                   2                   3
  //  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |  Message Type |  Channel Type |            Priority           |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                    Reliability Parameter                      |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |         Label Length          |       Protocol Length         |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                             Label                             |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                            Protocol                           |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  size_t label_len = strlen(label);
  size_t protocol_len = strlen(protocol);
  if (label_len > UINT16_MAX || protocol_len > UINT16_MAX ||
      label_len > SIZE_MAX - 12u - protocol_len) {
    (void)sctp_unregister_data_channel(&pc->sctp, sid);
    return rtrn;
  }
  size_t msg_size = 12u + label_len + protocol_len;
  uint8_t* msg = h2_pal_mem_alloc(pc->config.mem, msg_size);
  if (!msg) {
    (void)sctp_unregister_data_channel(&pc->sctp, sid);
    return rtrn;
  }
  size_t encoded_len = 0u;
  if (peer_connection_encode_datachannel_open(
          msg, msg_size, channel_type, priority, reliability_parameter, label,
          protocol, &encoded_len) == 0) {
    rtrn = sctp_outgoing_data(
        &pc->sctp, (char*)msg, encoded_len, PPID_CONTROL, sid);
  }
  h2_pal_mem_free(pc->config.mem, msg);
  if (rtrn < 0) {
    (void)sctp_unregister_data_channel(&pc->sctp, sid);
  }
  return rtrn;
}

static char* peer_connection_dtls_role_setup_value(
    DtlsSrtpRole d) {
  return d == DTLS_SRTP_ROLE_SERVER ? "a=setup:passive" : "a=setup:active";
}

static h2_pal_result_t peer_connection_loop_internal(
    PeerConnection* pc, uint32_t timeout_ms, int receive_completed) {
  h2_pal_result_t state_result = peer_connection_state_poll_result(pc->state);
  if (state_result != H2_PAL_OK) {
    return state_result;
  }
  h2_pal_result_t rtp_flush_result = peer_connection_flush_pending_rtp(pc);
  if (rtp_flush_result != H2_PAL_OK &&
      rtp_flush_result != H2_PAL_ERR_WOULD_BLOCK) {
    STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
    return rtp_flush_result;
  }
  h2_pal_result_t flush_result = dtls_srtp_flush_pending(&pc->dtls_srtp);
  if (flush_result != H2_PAL_OK && flush_result != H2_PAL_ERR_WOULD_BLOCK) {
    PeerConnectionState error_state = flush_result == H2_PAL_ERR_CLOSED
                                          ? PEER_CONNECTION_DISCONNECTED
                                          : PEER_CONNECTION_FAILED;
    STATE_CHANGED(pc, error_state);
    return flush_result;
  }
  if (sctp_service(&pc->sctp) != 0) {
    STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
    return H2_PAL_ERR_IO;
  }
  memset(pc->agent_buf, 0, sizeof(pc->agent_buf));
  pc->agent_ret = -1;

  switch (pc->state) {
    case PEER_CONNECTION_NEW:
      break;

    case PEER_CONNECTION_CHECKING:
      if (agent_select_candidate_pair(&pc->agent, timeout_ms) < 0) {
        STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
      } else if (agent_connectivity_check(&pc->agent, timeout_ms) == 0) {
        STATE_CHANGED(pc, PEER_CONNECTION_CONNECTED);
      }
      break;

    case PEER_CONNECTION_CONNECTED:
      pc->agent_ret = agent_recv(
          &pc->agent, pc->agent_buf, sizeof(pc->agent_buf), timeout_ms);
      if (pc->agent_ret < 0) {
        PeerConnectionState error_state = pc->agent_ret == H2_PAL_ERR_CLOSED
                                              ? PEER_CONNECTION_DISCONNECTED
                                              : PEER_CONNECTION_FAILED;
        STATE_CHANGED(pc, error_state);
        return (h2_pal_result_t)pc->agent_ret;
      }
      int handshake_result = dtls_srtp_handshake(
          &pc->dtls_srtp, pc->agent_ret > 0 ? pc->agent_buf : NULL,
          pc->agent_ret > 0 ? (size_t)pc->agent_ret : 0u);
      PeerConnectionState terminal_state = pc->state;
      h2_pal_result_t handshake_status =
          peer_connection_classify_dtls_handshake_result(
              handshake_result, &terminal_state);
      if (handshake_status != H2_PAL_OK) {
        STATE_CHANGED(pc, terminal_state);
        return handshake_status;
      }
      if (handshake_result == 0) {
        H2_PEER_LOGD(pc->config.log, "DTLS-SRTP handshake done");

        if (pc->config.datachannel) {
          H2_PEER_LOGI(pc->config.log, "SCTP create association");
          if (sctp_create_association(&pc->sctp, &pc->dtls_srtp) != 0) {
            STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
            return H2_PAL_ERR_IO;
          }
        }

        STATE_CHANGED(pc, PEER_CONNECTION_COMPLETED);
      }
      break;
    case PEER_CONNECTION_COMPLETED:
      if (agent_keepalive(&pc->agent) != 0) {
        STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
        return H2_PAL_ERR_IO;
      }
      if (receive_completed) {
        const uint32_t receive_timeout =
            timeout_ms < H2_PEER_COMPLETED_RECEIVE_TIMEOUT_MS
                ? timeout_ms
                : H2_PEER_COMPLETED_RECEIVE_TIMEOUT_MS;
        pc->agent_ret = agent_recv(
            &pc->agent, pc->agent_buf, sizeof(pc->agent_buf), receive_timeout);
        if (pc->agent_ret > 0) {
          H2_PEER_LOGD(pc->config.log, "agent_recv %d", pc->agent_ret);
          pc->agent_ret = peer_connection_process_completed_packet(
              pc, pc->agent_buf, pc->agent_ret);
        }
        if (pc->agent_ret < 0) {
          PeerConnectionState error_state = pc->agent_ret == H2_PAL_ERR_CLOSED
                                              ? PEER_CONNECTION_DISCONNECTED
                                              : PEER_CONNECTION_FAILED;
          STATE_CHANGED(pc, error_state);
          return (h2_pal_result_t)pc->agent_ret;
        }
      }

      uint64_t now_ms = 0u;
      if (CONFIG_KEEPALIVE_TIMEOUT > 0 &&
          h2_pal_time_get_monotonic_ms(pc->agent.time, &now_ms) == H2_PAL_OK &&
          now_ms >= pc->agent.ice_activity_time_ms &&
          now_ms - pc->agent.ice_activity_time_ms > CONFIG_KEEPALIVE_TIMEOUT) {
        H2_PEER_LOGI(pc->config.log, "binding request timeout");
        STATE_CHANGED(pc, PEER_CONNECTION_CLOSED);
        return H2_PAL_ERR_CLOSED;
      }

      break;
    case PEER_CONNECTION_FAILED:
      return H2_PAL_ERR_IO;
    case PEER_CONNECTION_DISCONNECTED:
    case PEER_CONNECTION_CLOSED:
      return H2_PAL_ERR_CLOSED;
    default:
      break;
  }

  return 0;
}

h2_pal_result_t peer_connection_loop(
    PeerConnection* pc, uint32_t timeout_ms) {
  return peer_connection_loop_internal(pc, timeout_ms, 1);
}

int peer_connection_receive_datagram(
    PeerConnection* pc, h2_pal_net_addr_t* addr, uint8_t* packet,
    size_t packet_cap, uint32_t timeout_ms) {
  if (pc == NULL || addr == NULL || packet == NULL || packet_cap == 0u ||
      packet_cap > (size_t)INT_MAX || pc->state != PEER_CONNECTION_COMPLETED) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return agent_recv_raw(&pc->agent, addr, packet, (int)packet_cap, timeout_ms);
}

int peer_connection_async_receive_supported(
    PeerConnection* pc) {
  return pc != NULL && pc->state == PEER_CONNECTION_COMPLETED &&
         agent_async_receive_supported(&pc->agent);
}

h2_pal_result_t peer_connection_service_datagram(
    PeerConnection* pc, h2_pal_net_addr_t* addr, uint8_t* packet,
    size_t packet_len) {
  if (pc == NULL || (packet == NULL && packet_len != 0u) ||
      (addr == NULL && packet_len != 0u) ||
      packet_len > sizeof(pc->agent_buf) ||
      pc->state != PEER_CONNECTION_COMPLETED) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_pal_result_t result = peer_connection_loop_internal(pc, 0u, 0);
  if (result != H2_PAL_OK || packet_len == 0u) {
    return result;
  }
  pc->agent_ret =
      agent_process_received(&pc->agent, addr, packet, (int)packet_len);
  if (pc->agent_ret > 0) {
    pc->agent_ret =
        peer_connection_process_completed_packet(pc, packet, pc->agent_ret);
  }
  if (pc->agent_ret < 0) {
    PeerConnectionState error_state = pc->agent_ret == H2_PAL_ERR_CLOSED
                                          ? PEER_CONNECTION_DISCONNECTED
                                          : PEER_CONNECTION_FAILED;
    STATE_CHANGED(pc, error_state);
    return (h2_pal_result_t)pc->agent_ret;
  }
  return H2_PAL_OK;
}

int peer_connection_set_remote_description(
    PeerConnection* pc, const char* sdp, SdpType type) {
  if (pc == NULL || sdp == NULL) {
    return -1;
  }
  char* start = (char*)sdp;
  char* line = NULL;
  char buf[256];
  char* val_start = NULL;
  uint32_t* ssrc = NULL;
  DtlsSrtpRole role = DTLS_SRTP_ROLE_SERVER;
  int is_update = 0;
  Agent* agent = &pc->agent;

  while ((line = strstr(start, "\r\n"))) {
    line = strstr(start, "\r\n");
    size_t line_len = (size_t)(line - start);
    if (line_len >= sizeof(buf)) {
      return -1;
    }
    memcpy(buf, start, line_len);
    buf[line_len] = '\0';

    if (strstr(buf, "a=setup:passive")) {
      role = DTLS_SRTP_ROLE_CLIENT;
    }

    if (strstr(buf, "a=fingerprint") &&
        dtls_srtp_set_remote_fingerprint(&pc->dtls_srtp, buf + 22) != 0) {
      return -1;
    }

    if (strstr(buf, "a=ice-ufrag") && strlen(agent->remote_ufrag) != 0 &&
        (strncmp(buf + strlen("a=ice-ufrag:"), agent->remote_ufrag,
                 strlen(agent->remote_ufrag)) == 0)) {
      is_update = 1;
    }

    if (strstr(buf, "m=video")) {
      ssrc = &pc->remote_vssrc;
    } else if (strstr(buf, "m=audio")) {
      ssrc = &pc->remote_assrc;
    }

    if ((val_start = strstr(buf, "a=ssrc:")) && ssrc) {
      *ssrc = strtoul(val_start + 7, NULL, 10);
      H2_PEER_LOGD(pc->config.log, "SSRC: %" PRIu32, *ssrc);
    }

    start = line + 2;
  }

  if (is_update) {
    return 0;
  }

  if (agent_set_remote_description(&pc->agent, (char*)sdp) != 0) {
    return -1;
  }
  if (type == SDP_TYPE_ANSWER) {
    STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
  }
  return 0;
}

static const char* peer_connection_create_sdp(
    PeerConnection* pc, SdpType sdp_type) {
  char* description = (char*)pc->temp_buf;

  memset(pc->temp_buf, 0, sizeof(pc->temp_buf));
  DtlsSrtpRole role = DTLS_SRTP_ROLE_SERVER;

  pc->sctp.connected = 0;

  switch (sdp_type) {
    case SDP_TYPE_OFFER:
      role = DTLS_SRTP_ROLE_SERVER;
      agent_clear_candidates(&pc->agent);
      pc->agent.mode = AGENT_MODE_CONTROLLING;
      break;
    case SDP_TYPE_ANSWER:
      role = DTLS_SRTP_ROLE_CLIENT;
      pc->agent.mode = AGENT_MODE_CONTROLLED;
      break;
    default:
      break;
  }

  dtls_srtp_reset_session(&pc->dtls_srtp);
  if (dtls_srtp_init(
          &pc->dtls_srtp, role, pc, pc->config.log, pc->config.dtls,
          pc->config.time) != 0) {
    return NULL;
  }
  pc->dtls_srtp.packet_send = peer_connection_dtls_srtp_send;

  memset(pc->sdp, 0, sizeof(pc->sdp));
  // TODO: check if we have video or audio codecs
  sdp_create(pc->sdp, pc->config.video_codec != CODEC_NONE,
             pc->config.audio_codec != CODEC_NONE, pc->config.datachannel);

  agent_create_ice_credential(&pc->agent);
  sdp_append(pc->sdp, "a=ice-ufrag:%s", pc->agent.local_ufrag);
  sdp_append(pc->sdp, "a=ice-pwd:%s", pc->agent.local_upwd);
  sdp_append(
      pc->sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
  sdp_append(pc->sdp, peer_connection_dtls_role_setup_value(role));

  if (pc->config.video_codec == CODEC_H264) {
    sdp_append_h264(pc->sdp);
  }

  switch (pc->config.audio_codec) {
    case CODEC_PCMA:
      sdp_append_pcma(pc->sdp);
      break;
    case CODEC_PCMU:
      sdp_append_pcmu(pc->sdp);
      break;
    case CODEC_OPUS:
      sdp_append_opus(pc->sdp);
    default:
      break;
  }

  if (pc->config.datachannel) {
    sdp_append_datachannel(pc->sdp);
  }

  pc->b_local_description_created = 1;

  if (agent_gather_candidate(&pc->agent, NULL, NULL, NULL) != 0) {
    return NULL;
  }
  for (int i = 0;
       i < sizeof(pc->config.ice_servers) / sizeof(pc->config.ice_servers[0]);
       ++i) {
    if (pc->config.ice_servers[i].urls) {
      H2_PEER_LOGI(pc->config.log, "ice server: %s",
                   pc->config.ice_servers[i].urls);
      if (agent_gather_candidate(&pc->agent, pc->config.ice_servers[i].urls,
                                 pc->config.ice_servers[i].username,
                                 pc->config.ice_servers[i].credential) != 0) {
        return NULL;
      }
    }
  }

  if (sdp_type == SDP_TYPE_ANSWER &&
      agent_update_candidate_pairs(&pc->agent) != 0) {
    return NULL;
  }

  agent_get_local_description(&pc->agent, description, sizeof(pc->temp_buf));
  sdp_append(pc->sdp, description);

  if (pc->onicecandidate) {
    pc->onicecandidate(pc->sdp, pc->config.user_data);
  }

  return pc->sdp;
}

const char* peer_connection_create_offer(
    PeerConnection* pc) {
  return peer_connection_create_sdp(pc, SDP_TYPE_OFFER);
}

const char* peer_connection_create_answer(
    PeerConnection* pc) {
  const char* sdp = peer_connection_create_sdp(pc, SDP_TYPE_ANSWER);
  if (sdp == NULL) {
    return NULL;
  }
  STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
  return sdp;
}

int peer_connection_send_rtcp_pil(
    PeerConnection* pc, uint32_t ssrc) {
  int ret = -1;
  uint8_t plibuf[128];
  rtcp_get_pli(plibuf, 12, ssrc);

  // TODO: encrypt rtcp packet
  // guint size = 12;
  // dtls_transport_encrypt_rctp_packet(pc->dtls_transport, plibuf, &size);
  // ret = nice_agent_send(pc->nice_agent, pc->stream_id, pc->component_id,
  // size, (gchar*)plibuf);

  return ret;
}

// callbacks
void peer_connection_on_connected(
    PeerConnection* pc, void (*on_connected)(void* userdata)) {
  pc->on_connected = on_connected;
}

void peer_connection_on_receiver_packet_loss(
    PeerConnection* pc,
    void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss,
                                    void* userdata)) {
  pc->on_receiver_packet_loss = on_receiver_packet_loss;
}

void peer_connection_onicecandidate(
    PeerConnection* pc, void (*onicecandidate)(char* sdp, void* userdata)) {
  pc->onicecandidate = onicecandidate;
}

void peer_connection_oniceconnectionstatechange(
    PeerConnection* pc,
    void (*oniceconnectionstatechange)(PeerConnectionState state,
                                       void* userdata)) {
  pc->oniceconnectionstatechange = oniceconnectionstatechange;
}

void peer_connection_ondatachannel(
    PeerConnection* pc,
    h2_pal_result_t (*onmessage)(char* msg, size_t len, void* userdata,
                                 uint16_t sid, int is_text),
    void (*onopen)(void* userdata), void (*onclose)(void* userdata)) {
  if (pc) {
    sctp_onopen(&pc->sctp, onopen);
    sctp_onclose(&pc->sctp, onclose);
    sctp_onmessage(&pc->sctp, onmessage);
  }
}

void peer_connection_onstreamreset(
    PeerConnection* pc,
    void (*onstreamreset)(const h2_pal_sctp_stream_reset_event_t* event,
                          void* userdata)) {
  if (pc != NULL) {
    sctp_onstreamreset(&pc->sctp, onstreamreset);
  }
}

void peer_connection_onlocalchannelopen(
    PeerConnection* pc,
    void (*onlocalchannelopen)(uint16_t sid, void* userdata)) {
  if (pc != NULL) {
    sctp_onlocalchannelopen(&pc->sctp, onlocalchannelopen);
  }
}

void peer_connection_onremotechannel(
    PeerConnection* pc,
    int (*onremotechannel)(const SctpRemoteChannel* channel, void* userdata)) {
  if (pc != NULL) {
    sctp_onremotechannel(&pc->sctp, onremotechannel);
  }
}

int peer_connection_lookup_sid(
    PeerConnection* pc, const char* label, uint16_t* sid) {
  for (size_t i = 0u; i < pc->sctp.stream_count; i++) {
    if (strncmp(pc->sctp.stream_table[i].label, label,
                sizeof(pc->sctp.stream_table[i].label)) == 0) {
      *sid = pc->sctp.stream_table[i].sid;
      return 0;
    }
  }
  return -1;  // Not found
}

char* peer_connection_lookup_sid_label(
    PeerConnection* pc, uint16_t sid) {
  for (size_t i = 0u; i < pc->sctp.stream_count; i++) {
    if (pc->sctp.stream_table[i].sid == sid) {
      return pc->sctp.stream_table[i].label;
    }
  }
  return NULL;  // Not found
}

int peer_connection_add_ice_candidate(
    PeerConnection* pc, char* candidate) {
  Agent* agent = &pc->agent;
  IceCandidate parsed;
  IceCandidateParseResult parse_result = ice_candidate_from_description(
      agent->net, &parsed, candidate, candidate + strlen(candidate));
  if (parse_result == ICE_CANDIDATE_PARSE_UNSUPPORTED) {
    return 0;
  }
  if (parse_result != ICE_CANDIDATE_PARSE_OK) {
    return -1;
  }
  int result = agent_add_remote_candidate(agent, &parsed);
  if (result == 0) {
    H2_PEER_LOGD(pc->config.log, "Added remote ICE candidate");
  }
  return result;
}
