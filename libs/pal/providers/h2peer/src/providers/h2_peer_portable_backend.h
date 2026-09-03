#ifndef H2_PEER_PORTABLE_BACKEND_H
#define H2_PEER_PORTABLE_BACKEND_H

#include "h2_peer.h"

h2_pal_result_t h2_peer_portable_start_offer(h2_pal_webrtc_peer_t *peer);
h2_pal_result_t h2_peer_portable_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                                                h2_pal_webrtc_sdp_type_t type,
                                                h2_pal_webrtc_str_t sdp);
h2_pal_result_t h2_peer_portable_poll(h2_pal_webrtc_peer_t *peer,
                                      int timeout_ms);
int h2_peer_portable_receive_datagram(h2_pal_webrtc_peer_t *peer,
                                      h2_pal_net_addr_t *addr, uint8_t *packet,
                                      size_t packet_cap, uint32_t timeout_ms);
h2_pal_result_t h2_peer_portable_service_datagram(h2_pal_webrtc_peer_t *peer,
                                                  h2_pal_net_addr_t *addr,
                                                  uint8_t *packet,
                                                  size_t packet_len);
int h2_peer_portable_async_receive_supported(const h2_pal_webrtc_peer_t *peer);
h2_pal_result_t h2_peer_portable_sctp_is_writable(
    h2_pal_webrtc_peer_t *peer, bool *out_writable);
h2_pal_result_t h2_peer_portable_send_opus(h2_pal_webrtc_peer_t *peer,
                                           const uint8_t *opus,
                                           size_t opus_len);
h2_pal_result_t h2_peer_portable_channel_open(h2_pal_webrtc_channel_t *channel);
h2_pal_result_t h2_peer_portable_channel_send(h2_pal_webrtc_channel_t *channel,
                                              const uint8_t *data, size_t len,
                                              int is_text);
h2_pal_result_t h2_peer_portable_reset_stream(h2_pal_webrtc_peer_t *peer,
                                              uint16_t stream_id);

h2_pal_result_t h2_peer_portable_forget_stream(h2_pal_webrtc_peer_t *peer,
                                               uint16_t stream_id);
void h2_peer_portable_peer_close(h2_pal_webrtc_peer_t *peer);

#endif
