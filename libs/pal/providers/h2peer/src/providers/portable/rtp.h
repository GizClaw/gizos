#ifndef H2_PEER_PORTABLE_RTP_H
#define H2_PEER_PORTABLE_RTP_H

#include "config.h"
#include "peer_connection.h"

#include <stddef.h>
#include <stdint.h>

#define H2_PEER_PORTABLE_RTP_HEADER_SIZE 12u

typedef enum RtpPayloadType {
  PT_PCMU = 0,
  PT_PCMA = 8,
  PT_G722 = 9,
  PT_H264 = 96,
  PT_OPUS = 111
} RtpPayloadType;

typedef enum RtpSsrc {
  SSRC_H264 = 1,
  SSRC_PCMA = 4,
  SSRC_PCMU = 5,
  SSRC_OPUS = 6
} RtpSsrc;

typedef struct RtpMap {
  int pt_h264;
  int pt_opus;
  int pt_pcma;
} RtpMap;

typedef struct RtpEncoder RtpEncoder;
typedef struct RtpDecoder RtpDecoder;
typedef int (*RtpOnEncodedPacket)(
    uint8_t *packet, size_t bytes, void *user_data);
typedef void (*RtpOnDecodedPacket)(
    uint8_t *packet, size_t bytes, void *user_data);

typedef enum RtpDecodeEvent {
  RTP_DECODE_EVENT_NONE = 0,
  RTP_DECODE_EVENT_PACKET,
  RTP_DECODE_EVENT_REORDER_WAIT,
  RTP_DECODE_EVENT_REORDER_RECOVERED,
  RTP_DECODE_EVENT_LOSS,
  RTP_DECODE_EVENT_LATE,
  RTP_DECODE_EVENT_RESET
} RtpDecodeEvent;

struct RtpDecoder {
  RtpPayloadType type;
  RtpOnDecodedPacket on_packet;
  void *user_data;
  uint32_t ssrc;
  uint32_t last_timestamp;
  uint16_t next_sequence;
  uint16_t last_loss_count;
  uint16_t reorder_sequence;
  uint32_t reorder_timestamp;
  size_t reorder_payload_size;
  RtpDecodeEvent last_event;
  uint8_t sequence_initialized;
  uint8_t reorder_pending;
  uint8_t reorder_payload[CONFIG_MTU];
};

struct RtpEncoder {
  RtpPayloadType type;
  RtpOnEncodedPacket on_packet;
  void *user_data;
  uint16_t seq_number;
  uint32_t ssrc;
  uint32_t timestamp;
  uint32_t timestamp_increment;
  uint8_t buf[CONFIG_MTU + 128];
};

int rtp_packet_validate(uint8_t *packet, size_t size);
void rtp_encoder_init(RtpEncoder *encoder, MediaCodec codec, RtpOnEncodedPacket on_packet, void *user_data);
int rtp_encoder_encode(RtpEncoder *encoder, const uint8_t *data, size_t size);
void rtp_decoder_init(RtpDecoder *decoder, MediaCodec codec, RtpOnDecodedPacket on_packet, void *user_data);
int rtp_decoder_decode(RtpDecoder *decoder, const uint8_t *data, size_t size);
int rtp_decoders_decode(RtpDecoder *audio_decoder,
                        RtpDecoder *video_decoder,
                        const uint8_t *data,
                        size_t size);
uint32_t rtp_get_ssrc(uint8_t *packet);

#endif
