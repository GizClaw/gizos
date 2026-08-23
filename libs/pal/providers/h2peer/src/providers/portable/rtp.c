#include "rtp.h"

#include <string.h>

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
  size_t offset = h2_peer_rtp_payload_offset(data, size);
  if (offset == 0u || (data[1] & 0x7fu) != (uint8_t)decoder->type) {
    return -1;
  }
  if (decoder->on_packet != NULL) {
    decoder->on_packet((uint8_t *)&data[offset], size - offset, decoder->user_data);
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
