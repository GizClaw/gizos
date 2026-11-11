#ifndef H2_BK_NET_RTP_TRACE_H
#define H2_BK_NET_RTP_TRACE_H

#include "core/h2_pal_errors.h"
#include <stddef.h>
#include <stdint.h>

/* Private qualification observer. Only RTP header metadata is retained;
 * payload, keys and addresses are never copied. Call stop before releasing the
 * borrowed PSRAM buffer. The opt-in native component option leaves normal
 * images alone. */
enum { H2_BK_RTP_TRACE_SEND = 1, H2_BK_RTP_TRACE_RECEIVE = 2 };

typedef struct h2_bk_net_rtp_trace_event {
  uint64_t started_us, completed_us;
  uint32_t ssrc, rtp_timestamp;
  int fd, result, socket_errno;
  uint16_t sequence, packet_bytes;
  uint8_t direction, payload_type;
} h2_bk_net_rtp_trace_event_t;

typedef struct h2_bk_net_rtp_trace_stats {
  size_t count, overflow;
  uint64_t send_calls, receive_calls;
  uint64_t max_send_us, max_receive_us;
  uint64_t max_send_started_us, max_receive_started_us;
} h2_bk_net_rtp_trace_stats_t;

h2_pal_result_t h2_bk_net_rtp_trace_start(h2_bk_net_rtp_trace_event_t* records,
                                          size_t capacity);
h2_pal_result_t h2_bk_net_rtp_trace_stop(
    h2_bk_net_rtp_trace_stats_t* out_stats);

#endif
