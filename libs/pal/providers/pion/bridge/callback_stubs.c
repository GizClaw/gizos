#include "h2_pion_bridge.h"

/*
 * A c-archive must resolve its reverse C callback references while linking.
 * Final consumers also link h2_pion.c, whose strong definitions replace
 * these stubs and project queued Go events into PAL callbacks from peer_poll.
 */
#if defined(__GNUC__) || defined(__clang__)
#define H2_PION_WEAK __attribute__((weak))
#else
#define H2_PION_WEAK
#endif

H2_PION_WEAK void h2_pion_bridge_emit_peer_state(uintptr_t peer_key, int state) {
  (void)peer_key;
  (void)state;
}

H2_PION_WEAK int
h2_pion_bridge_emit_channel_open(uintptr_t peer_key, uint64_t channel_key,
                              const char *label, size_t label_len,
                              int has_stream_id, uint16_t stream_id,
                              int ordered, int reliable, int remote) {
  (void)peer_key;
  (void)channel_key;
  (void)label;
  (void)label_len;
  (void)has_stream_id;
  (void)stream_id;
  (void)ordered;
  (void)reliable;
  (void)remote;
  return 0;
}

H2_PION_WEAK void h2_pion_bridge_emit_channel_state(uintptr_t peer_key,
                                                 uint64_t channel_key,
                                                 int state) {
  (void)peer_key;
  (void)channel_key;
  (void)state;
}

H2_PION_WEAK void h2_pion_bridge_emit_channel_message(uintptr_t peer_key,
                                                   uint64_t channel_key,
                                                   const uint8_t *data,
                                                   size_t len, int is_text) {
  (void)peer_key;
  (void)channel_key;
  (void)data;
  (void)len;
  (void)is_text;
}

H2_PION_WEAK void h2_pion_bridge_emit_opus_frame(uintptr_t peer_key,
                                              const uint8_t *data, size_t len) {
  (void)peer_key;
  (void)data;
  (void)len;
}
