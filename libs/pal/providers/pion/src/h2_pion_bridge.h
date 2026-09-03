#ifndef H2_PION_BRIDGE_H
#define H2_PION_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int h2_pion_bridge_emit_peer_state(uintptr_t peer_key, int state);
int h2_pion_bridge_emit_writable(uintptr_t peer_key, uint64_t channel_key);
int h2_pion_bridge_emit_channel_open(uintptr_t peer_key, uint64_t channel_key,
                                     const char *label, size_t label_len,
                                     int has_stream_id, uint16_t stream_id,
                                     int ordered, int reliable, int remote);
int h2_pion_bridge_emit_channel_state(uintptr_t peer_key, uint64_t channel_key,
                                      int state);
int h2_pion_bridge_emit_channel_message(uintptr_t peer_key,
                                        uint64_t channel_key,
                                        const uint8_t *data, size_t len,
                                        int is_text);
int h2_pion_bridge_emit_opus_frame(uintptr_t peer_key, const uint8_t *data,
                                   size_t len);

#ifdef __cplusplus
}
#endif

#endif
