/**
 * @file peer_connection.h
 * @brief Struct PeerConnection
 */
#ifndef PEER_CONNECTION_H_
#define PEER_CONNECTION_H_

#include <stdint.h>
#include <stdlib.h>

#include "h2/pal/net/h2_pal_dtls.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/net/h2_pal_sctp.h"
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SdpType {
  SDP_TYPE_OFFER = 0,
  SDP_TYPE_ANSWER,
} SdpType;

typedef enum PeerConnectionState {

  PEER_CONNECTION_CLOSED = 0,
  PEER_CONNECTION_NEW,
  PEER_CONNECTION_CHECKING,
  PEER_CONNECTION_CONNECTED,
  PEER_CONNECTION_COMPLETED,
  PEER_CONNECTION_FAILED,
  PEER_CONNECTION_DISCONNECTED,

} PeerConnectionState;

typedef enum DataChannelType {

  DATA_CHANNEL_NONE = 0,
  DATA_CHANNEL_STRING,
  DATA_CHANNEL_BINARY,

} DataChannelType;

typedef enum DecpChannelType {
  DATA_CHANNEL_RELIABLE = 0x00,
  DATA_CHANNEL_RELIABLE_UNORDERED = 0x80,
  DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT = 0x01,
  DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED = 0x81,
  DATA_CHANNEL_PARTIAL_RELIABLE_TIMED = 0x02,
  DATA_CHANNEL_PARTIAL_RELIABLE_TIMED_UNORDERED = 0x82,
} DecpChannelType;

typedef enum MediaCodec {

  CODEC_NONE = 0,

  /* Video */
  CODEC_H264,
  CODEC_VP8,    // not implemented yet
  CODEC_MJPEG,  // not implemented yet

  /* Audio */
  CODEC_OPUS,  // not implemented yet
  CODEC_PCMA,
  CODEC_PCMU,

} MediaCodec;

typedef struct IceServer {
  const char* urls;
  const char* username;
  const char* credential;

} IceServer;

typedef struct PeerConfiguration {
  const h2_pal_log_api_t *log;
  const h2_pal_mem_api_t *mem;
  const h2_pal_net_api_t *net;
  const h2_pal_time_api_t *time;
  const h2_pal_crypto_api_t* crypto;
  const h2_pal_dtls_api_t* dtls;
  const h2_pal_sctp_api_t* sctp;

  IceServer ice_servers[5];

  MediaCodec audio_codec;
  MediaCodec video_codec;
  DataChannelType datachannel;

  void (*onaudiotrack)(uint8_t* data, size_t size, void* userdata);
  void (*onvideotrack)(uint8_t* data, size_t size, void* userdata);
  void (*on_request_keyframe)(void* userdata);
  void* user_data;

} PeerConfiguration;

typedef struct PeerConnection PeerConnection;
typedef struct SctpRemoteChannel SctpRemoteChannel;
typedef int (*PeerConnectionRtpProtectFn)(void* user, uint8_t* packet,
                                         int* packet_len);
typedef int (*PeerConnectionRtpSendFn)(void* user, const uint8_t* packet,
                                      int packet_len);

const char* peer_connection_state_to_string(PeerConnectionState state);

PeerConnectionState peer_connection_get_state(PeerConnection* pc);

h2_pal_result_t peer_connection_classify_dtls_handshake_result(
    int handshake_result, PeerConnectionState* out_terminal_state);

h2_pal_result_t peer_connection_state_poll_result(PeerConnectionState state);

int peer_connection_rtp_send_or_queue(
    uint8_t* pending_packet, size_t pending_capacity, size_t* pending_len,
    uint8_t* packet, size_t packet_len, PeerConnectionRtpProtectFn protect,
    void* protect_user, PeerConnectionRtpSendFn send, void* send_user);

h2_pal_result_t peer_connection_rtp_flush(uint8_t* pending_packet,
                                          size_t* pending_len,
                                          PeerConnectionRtpSendFn send,
                                          void* send_user);

void* peer_connection_get_sctp(PeerConnection* pc);
h2_pal_result_t peer_connection_sctp_is_writable(PeerConnection* pc,
                                                  bool* out_writable);

PeerConnection* peer_connection_create(PeerConfiguration* config);

void peer_connection_destroy(PeerConnection* pc);

void peer_connection_close(PeerConnection* pc);

h2_pal_result_t peer_connection_loop(PeerConnection* pc, uint32_t timeout_ms);

int peer_connection_receive_datagram(PeerConnection* pc,
                                     h2_pal_net_addr_t* addr, uint8_t* packet,
                                     size_t packet_cap, uint32_t timeout_ms);

h2_pal_result_t peer_connection_service_datagram(PeerConnection* pc,
                                                 h2_pal_net_addr_t* addr,
                                                 uint8_t* packet,
                                                 size_t packet_len);
int peer_connection_async_receive_supported(PeerConnection* pc);

int peer_connection_create_datachannel(PeerConnection* pc,
                                       DecpChannelType channel_type,
                                       uint16_t priority,
                                       uint32_t reliability_parameter,
                                       char* label, char* protocol);

int peer_connection_create_datachannel_sid(
    PeerConnection* pc, DecpChannelType channel_type, uint16_t priority,
    uint32_t reliability_parameter, char* label, char* protocol, uint16_t sid);

/**
 * @brief send message to data channel
 * @param[in] peer connection
 * @param[in] message buffer
 * @param[in] length of message
 */
int peer_connection_datachannel_send(PeerConnection* pc, char* message,
                                     size_t len);

int peer_connection_datachannel_send_sid(PeerConnection* pc, char* message,
                                         size_t len, uint16_t sid);

h2_pal_result_t peer_connection_datachannel_close_sid(PeerConnection* pc,
                                                      uint16_t sid);

int peer_connection_datachannel_forget_sid(PeerConnection* pc, uint16_t sid);

int peer_connection_send_audio(PeerConnection* pc, const uint8_t* packet,
                               size_t bytes);

int peer_connection_send_video(PeerConnection* pc, const uint8_t* packet,
                               size_t bytes);

int peer_connection_set_remote_description(PeerConnection* pc, const char* sdp,
                                           SdpType sdp_type);

int peer_connection_encode_datachannel_open(
    uint8_t* out, size_t out_cap, DecpChannelType channel_type,
    uint16_t priority, uint32_t reliability_parameter, const char* label,
    const char* protocol, size_t* out_len);

void peer_connection_set_local_description(PeerConnection* pc, const char* sdp,
                                           SdpType sdp_type);

const char* peer_connection_create_offer(PeerConnection* pc);

const char* peer_connection_create_answer(PeerConnection* pc);

/**
 * @brief register callback function to handle packet loss from RTCP receiver
 * report
 * @param[in] peer connection
 * @param[in] callback function void (*cb)(float fraction_loss, uint32_t
 * total_loss, void *userdata)
 * @param[in] userdata for callback function
 */
void peer_connection_on_receiver_packet_loss(
    PeerConnection* pc,
    void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss,
                                    void* userdata));

/**
 * @brief Set the callback function to handle onicecandidate event.
 * @param A PeerConnection.
 * @param A callback function to handle onicecandidate event.
 * @param A userdata which is pass to callback function.
 */
void peer_connection_onicecandidate(PeerConnection* pc,
                                    void (*onicecandidate)(char* sdp_text,
                                                           void* userdata));

/**
 * @brief Set the callback function to handle oniceconnectionstatechange event.
 * @param A PeerConnection.
 * @param A callback function to handle oniceconnectionstatechange event.
 * @param A userdata which is pass to callback function.
 */
void peer_connection_oniceconnectionstatechange(
    PeerConnection* pc,
    void (*oniceconnectionstatechange)(PeerConnectionState state,
                                       void* userdata));

/**
 * @brief register callback function to handle event of datachannel
 * @param[in] peer connection
 * @param[in] callback function when message received
 * @param[in] callback function when connection is opened
 * @param[in] callback function when connection is closed
 */
void peer_connection_ondatachannel(PeerConnection* pc,
                                   h2_pal_result_t (*onmessage)(
                                       char* msg, size_t len, void* userdata,
                                       uint16_t sid, int is_text),
                                   void (*onopen)(void* userdata),
                                   void (*onclose)(void* userdata));

void peer_connection_onstreamreset(
    PeerConnection* pc,
    void (*onstreamreset)(const h2_pal_sctp_stream_reset_event_t* event,
                          void* userdata));

void peer_connection_onlocalchannelopen(
    PeerConnection* pc,
    void (*onlocalchannelopen)(uint16_t sid, void* userdata));

void peer_connection_onremotechannel(
    PeerConnection* pc,
    int (*onremotechannel)(const SctpRemoteChannel* channel, void* userdata));

int peer_connection_lookup_sid(PeerConnection* pc, const char* label,
                               uint16_t* sid);

char* peer_connection_lookup_sid_label(PeerConnection* pc, uint16_t sid);

/**
 * @brief adds a new remote candidate to the peer connection
 * @param[in] peer connection
 * @param[in] ice candidate
 */
int peer_connection_add_ice_candidate(PeerConnection* pc, char* ice_candidate);

#ifdef __cplusplus
}
#endif

#endif  // PEER_CONNECTION_H_
