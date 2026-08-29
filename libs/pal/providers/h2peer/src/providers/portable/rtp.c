#include "rtp.h"

#include <string.h>

#define H2_PEER_PORTABLE_RTP_MAX_CONCEALED_PACKETS 50u

static void h2_peer_rtp_deliver(RtpDecoder *decoder,
                                const uint8_t *payload,
                                size_t payload_size) {
  if (decoder->on_packet != NULL) {
    decoder->on_packet((uint8_t *)payload, payload_size, decoder->user_data);
  }
}

static void h2_peer_rtp_deliver_loss(RtpDecoder *decoder, uint16_t count) {
  if (decoder->on_packet == NULL) {
    return;
  }
  for (uint16_t missing = 0u; missing < count; ++missing) {
    decoder->on_packet(NULL, 0u, decoder->user_data);
  }
}

static uint16_t h2_peer_read_be16(const uint8_t *data) {
  return (uint16_t)(((uint16_t)data[0] << 8u) | data[1]);
}

static uint32_t h2_peer_read_be32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
         ((uint32_t)data[2] << 8u) | data[3];
}

static void h2_peer_write_be16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)(value >> 8u);
  data[1] = (uint8_t)value;
}

static void h2_peer_write_be32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value >> 24u);
  data[1] = (uint8_t)(value >> 16u);
  data[2] = (uint8_t)(value >> 8u);
  data[3] = (uint8_t)value;
}

static size_t h2_peer_rtp_payload_offset(const uint8_t *packet, size_t size) {
  if (size < H2_PEER_PORTABLE_RTP_HEADER_SIZE || (packet[0] >> 6u) != 2u) {
    return 0u;
  }
  size_t offset = H2_PEER_PORTABLE_RTP_HEADER_SIZE +
                  ((size_t)(packet[0] & 0x0fu) * 4u);
  if (offset > size) {
    return 0u;
  }
  if ((packet[0] & 0x10u) != 0u) {
    if (size - offset < 4u) {
      return 0u;
    }
    size_t extension = 4u + ((size_t)h2_peer_read_be16(&packet[offset + 2u]) * 4u);
    if (extension > size - offset) {
      return 0u;
    }
    offset += extension;
  }
  return offset;
}

int rtp_packet_validate(uint8_t *packet, size_t size) {
  if (packet == NULL || h2_peer_rtp_payload_offset(packet, size) == 0u) {
    return 0;
  }
  uint8_t payload_type = packet[1] & 0x7fu;
  return payload_type < 64u || payload_type >= 96u;
}

uint32_t rtp_get_ssrc(uint8_t *packet) {
  return packet == NULL ? 0u : h2_peer_read_be32(&packet[8]);
}

void rtp_encoder_init(RtpEncoder *encoder, MediaCodec codec, RtpOnEncodedPacket on_packet, void *user_data) {
  memset(encoder, 0, sizeof(*encoder));
  encoder->on_packet = on_packet;
  encoder->user_data = user_data;
  switch (codec) {
    case CODEC_PCMA:
      encoder->type = PT_PCMA;
      encoder->ssrc = SSRC_PCMA;
      encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8u;
      break;
    case CODEC_PCMU:
      encoder->type = PT_PCMU;
      encoder->ssrc = SSRC_PCMU;
      encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8u;
      break;
    case CODEC_OPUS:
      encoder->type = PT_OPUS;
      encoder->ssrc = SSRC_OPUS;
      encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 48u;
      break;
    default:
      encoder->type = PT_H264;
      encoder->ssrc = SSRC_H264;
      encoder->timestamp_increment = 3000u;
      break;
  }
}

int rtp_encoder_encode(RtpEncoder *encoder, const uint8_t *data, size_t size) {
  if (encoder == NULL || encoder->on_packet == NULL ||
      (data == NULL && size != 0u) ||
      size > sizeof(encoder->buf) - H2_PEER_PORTABLE_RTP_HEADER_SIZE) {
    return -1;
  }
  encoder->buf[0] = 0x80u;
  encoder->buf[1] = (uint8_t)encoder->type;
  h2_peer_write_be16(&encoder->buf[2], encoder->seq_number);
  h2_peer_write_be32(&encoder->buf[4], encoder->timestamp);
  h2_peer_write_be32(&encoder->buf[8], encoder->ssrc);
  memcpy(&encoder->buf[H2_PEER_PORTABLE_RTP_HEADER_SIZE], data, size);
  int result = encoder->on_packet(
      encoder->buf,
      H2_PEER_PORTABLE_RTP_HEADER_SIZE + size,
      encoder->user_data);
  if (result < 0) {
    return result;
  }
  encoder->seq_number++;
  encoder->timestamp += encoder->timestamp_increment;
  return 0;
}

void rtp_decoder_init(RtpDecoder *decoder, MediaCodec codec, RtpOnDecodedPacket on_packet, void *user_data) {
  memset(decoder, 0, sizeof(*decoder));
  decoder->type = codec == CODEC_OPUS ? PT_OPUS :
                  codec == CODEC_PCMA ? PT_PCMA :
                  codec == CODEC_PCMU ? PT_PCMU : PT_H264;
  decoder->on_packet = on_packet;
  decoder->user_data = user_data;
}

int rtp_decoder_decode(RtpDecoder *decoder, const uint8_t *data, size_t size) {
  if (decoder == NULL || data == NULL) {
    return -1;
  }
  decoder->last_event = RTP_DECODE_EVENT_NONE;
  decoder->last_loss_count = 0u;
  size_t offset = h2_peer_rtp_payload_offset(data, size);
  if (offset == 0u || (data[1] & 0x7fu) != (uint8_t)decoder->type) {
    return -1;
  }
  const uint16_t sequence = h2_peer_read_be16(&data[2]);
  const uint32_t timestamp = h2_peer_read_be32(&data[4]);
  const uint32_t ssrc = h2_peer_read_be32(&data[8]);
  const size_t payload_size = size - offset;
  if (!decoder->sequence_initialized || decoder->ssrc != ssrc) {
    decoder->sequence_initialized = 1u;
    decoder->ssrc = ssrc;
    decoder->next_sequence = sequence;
    decoder->last_timestamp = timestamp;
    decoder->reorder_pending = 0u;
    decoder->last_event = RTP_DECODE_EVENT_RESET;
  } else {
    const uint16_t ahead = (uint16_t)(sequence - decoder->next_sequence);
    if (ahead != 0u && ahead < UINT16_C(0x8000)) {
      if (ahead > H2_PEER_PORTABLE_RTP_MAX_CONCEALED_PACKETS) {
        decoder->next_sequence = sequence;
        decoder->reorder_pending = 0u;
        decoder->last_event = RTP_DECODE_EVENT_RESET;
      } else if (decoder->type == PT_OPUS && !decoder->reorder_pending &&
                 payload_size <= sizeof(decoder->reorder_payload)) {
        memcpy(decoder->reorder_payload, &data[offset], payload_size);
        decoder->reorder_payload_size = payload_size;
        decoder->reorder_sequence = sequence;
        decoder->reorder_timestamp = timestamp;
        decoder->reorder_pending = 1u;
        decoder->last_event = RTP_DECODE_EVENT_REORDER_WAIT;
        return (int)size;
      } else {
        decoder->last_event = RTP_DECODE_EVENT_LOSS;
        uint16_t confirmed_ahead = ahead;
        if (decoder->reorder_pending) {
          confirmed_ahead =
              (uint16_t)(decoder->reorder_sequence - decoder->next_sequence);
          h2_peer_rtp_deliver_loss(decoder, confirmed_ahead);
          h2_peer_rtp_deliver(decoder,
                              decoder->reorder_payload,
                              decoder->reorder_payload_size);
          decoder->next_sequence = (uint16_t)(decoder->reorder_sequence + 1u);
          decoder->last_timestamp = decoder->reorder_timestamp;
          decoder->reorder_pending = 0u;
          decoder->last_loss_count = confirmed_ahead;
          if (sequence == decoder->next_sequence) {
            h2_peer_rtp_deliver(decoder, &data[offset], payload_size);
            decoder->next_sequence = (uint16_t)(sequence + 1u);
            decoder->last_timestamp = timestamp;
            return (int)size;
          }
          if ((uint16_t)(sequence - decoder->next_sequence) >=
              UINT16_C(0x8000)) {
            return (int)size;
          }
          if (decoder->type == PT_OPUS &&
              payload_size <= sizeof(decoder->reorder_payload)) {
            memcpy(decoder->reorder_payload, &data[offset], payload_size);
            decoder->reorder_payload_size = payload_size;
            decoder->reorder_sequence = sequence;
            decoder->reorder_timestamp = timestamp;
            decoder->reorder_pending = 1u;
            return (int)size;
          }
          confirmed_ahead =
              (uint16_t)(sequence - decoder->next_sequence);
        }
        decoder->last_loss_count = confirmed_ahead;
        h2_peer_rtp_deliver_loss(decoder, confirmed_ahead);
      }
    } else if (ahead >= UINT16_C(0x8000)) {
      const uint32_t timestamp_ahead = timestamp - decoder->last_timestamp;
      if (timestamp_ahead < UINT32_C(0x80000000) &&
          timestamp_ahead != 0u) {
        decoder->next_sequence = sequence;
        decoder->reorder_pending = 0u;
        decoder->last_event = RTP_DECODE_EVENT_RESET;
      } else {
        decoder->last_event = RTP_DECODE_EVENT_LATE;
        return (int)size;
      }
    }
  }
  h2_peer_rtp_deliver(decoder, &data[offset], payload_size);
  if (decoder->last_event == RTP_DECODE_EVENT_NONE) {
    decoder->last_event = RTP_DECODE_EVENT_PACKET;
  }
  decoder->next_sequence = (uint16_t)(sequence + 1u);
  decoder->last_timestamp = timestamp;
  if (decoder->reorder_pending &&
      decoder->reorder_sequence == decoder->next_sequence) {
    h2_peer_rtp_deliver(decoder,
                        decoder->reorder_payload,
                        decoder->reorder_payload_size);
    decoder->next_sequence = (uint16_t)(decoder->reorder_sequence + 1u);
    decoder->last_timestamp = decoder->reorder_timestamp;
    decoder->reorder_pending = 0u;
    decoder->last_event = RTP_DECODE_EVENT_REORDER_RECOVERED;
  }
  return (int)size;
}

int rtp_decoders_decode(RtpDecoder *audio_decoder,
                        RtpDecoder *video_decoder,
                        const uint8_t *data,
                        size_t size) {
  if (data == NULL) {
    return -1;
  }
  if (audio_decoder != NULL &&
      rtp_decoder_decode(audio_decoder, data, size) >= 0) {
    return (int)size;
  }
  if (video_decoder != NULL &&
      rtp_decoder_decode(video_decoder, data, size) >= 0) {
    return (int)size;
  }
  return -1;
}
