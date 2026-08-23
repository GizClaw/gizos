#include <stdio.h>
#include <string.h>

#include "rtcp.h"

static uint16_t rtcp_read_be16(const uint8_t* data) {
  return (uint16_t)(((uint16_t)data[0] << 8u) | data[1]);
}

static void rtcp_write_be16(uint8_t* data, uint16_t value) {
  data[0] = (uint8_t)(value >> 8u);
  data[1] = (uint8_t)value;
}

static void rtcp_write_be32(uint8_t* data, uint32_t value) {
  data[0] = (uint8_t)(value >> 24u);
  data[1] = (uint8_t)(value >> 16u);
  data[2] = (uint8_t)(value >> 8u);
  data[3] = (uint8_t)value;
}

int rtcp_parse_header(
    const uint8_t* packet, size_t size, RtcpHeader* header) {
  if (packet == NULL || header == NULL || size < 4u) {
    return -1;
  }
  header->version = packet[0] >> 6u;
  header->padding = (packet[0] >> 5u) & 1u;
  header->rc = packet[0] & 0x1fu;
  header->type = packet[1];
  header->length = rtcp_read_be16(&packet[2]);
  size_t packet_size = ((size_t)header->length + 1u) * 4u;
  return header->version == 2u && packet_size >= 4u && packet_size <= size
             ? 0
             : -1;
}

int rtcp_probe(uint8_t* packet, size_t size) {
  RtcpHeader header;
  if (size < 8u || rtcp_parse_header(packet, size, &header) != 0)
    return 0;
  return header.type >= RTCP_SR && header.type <= 223u;
}

int rtcp_get_pli(uint8_t* packet, int len, uint32_t ssrc) {
  if (packet == NULL || len != 12)
    return -1;

  memset(packet, 0, len);
  packet[0] = 0x81u;
  packet[1] = RTCP_PSFB;
  rtcp_write_be16(&packet[2], (uint16_t)((len / 4) - 1));
  rtcp_write_be32(&packet[8], ssrc);

  return 12;
}

int rtcp_get_fir(uint8_t* packet, int len, int* seqnr) {
  if (packet == NULL || len != 20 || seqnr == NULL)
    return -1;

  memset(packet, 0, len);
  *seqnr = *seqnr + 1;
  if (*seqnr < 0 || *seqnr >= 256)
    *seqnr = 0;

  packet[0] = 0x84u;
  packet[1] = RTCP_PSFB;
  rtcp_write_be16(&packet[2], (uint16_t)((len / 4) - 1));
  packet[16] = (uint8_t)*seqnr;

  return 20;
}
