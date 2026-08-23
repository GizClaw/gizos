#ifndef RTCP_H_
#define RTCP_H_

#include <stddef.h>
#include <stdint.h>

typedef enum RtcpType {

  RTCP_FIR = 192,
  RTCP_SR = 200,
  RTCP_RR = 201,
  RTCP_SDES = 202,
  RTCP_BYE = 203,
  RTCP_APP = 204,
  RTCP_RTPFB = 205,
  RTCP_PSFB = 206,
  RTCP_XR = 207,

} RtcpType;

typedef struct RtcpHeader {
  uint8_t version;
  uint8_t padding;
  uint8_t rc;
  uint8_t type;
  uint16_t length;
} RtcpHeader;

int rtcp_probe(uint8_t* packet, size_t size);

int rtcp_parse_header(
    const uint8_t* packet, size_t size, RtcpHeader* header);

int rtcp_get_pli(uint8_t* packet, int len, uint32_t ssrc);

int rtcp_get_fir(uint8_t* packet, int len, int* seqnr);

#endif  // RTCP_H_
