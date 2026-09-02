#include "h2_pion.h"
#include "h2_pion_bridge.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_state {
  size_t track_reads;
} test_state_t;
static int s_fail_next_alloc;

static void *test_alloc(void *user, size_t len) {
  (void)user;
  if (s_fail_next_alloc) {
    s_fail_next_alloc = 0;
    return NULL;
  }
  return malloc(len);
}
static void *test_realloc(void *user, void *ptr, size_t len) {
  (void)user;
  return realloc(ptr, len);
}
static void test_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static int contains_bytes(const char *data, size_t len, const char *needle,
                          size_t needle_len) {
  if (needle_len == 0u || needle_len > len)
    return 0;
  for (size_t offset = 0u; offset <= len - needle_len; ++offset)
    if (memcmp(data + offset, needle, needle_len) == 0)
      return 1;
  return 0;
}

static h2_pal_result_t test_track_read(void *user, uint8_t *opus,
                                       size_t capacity, size_t *out_len) {
  test_state_t *test = user;
  if (test->track_reads != 0u)
    return H2_PAL_ERR_WOULD_BLOCK;
  assert(capacity >= 2u);
  opus[0] = 0xf8u;
  opus[1] = 0x42u;
  *out_len = 2u;
  test->track_reads++;
  return H2_PAL_OK;
}

int main(void) {
  static const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc, .realloc = test_realloc, .free = test_free};
  h2_pal_mem_api_t mem = {.user = NULL, .vtable = &mem_vtable};
  h2_pion_t *provider = NULL;
  assert(h2_pion_create(NULL, &provider) == H2_PAL_ERR_INVALID_ARG);
  h2_pion_config_t config = {.mem = &mem};
  assert(h2_pion_create(&config, &provider) == H2_PAL_OK);
  const h2_pal_webrtc_api_t *api = h2_pion_webrtc_api(provider);
  assert(api != NULL);

  h2_pal_webrtc_peer_t *peer = NULL;
  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK);
  test_state_t test = {0};
  static const h2_pal_webrtc_track_vtable_t track_vtable = {
      .read = test_track_read};
  h2_pal_webrtc_track_t track = {.user = &test, .vtable = &track_vtable};
  assert(h2_pal_webrtc_peer_set_track(api, peer, &track) == H2_PAL_OK);

  h2_pion_bridge_emit_peer_state((uintptr_t)peer, H2_PAL_WEBRTC_PEER_CONNECTED);
  h2_pal_webrtc_event_t event = {0};
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_PEER_STATE);
  assert(event.peer_state == H2_PAL_WEBRTC_PEER_CONNECTED);
  h2_pal_webrtc_event_release(&event);

  h2_pion_test_block_next_opus_send(peer);
  (void)h2_pal_webrtc_peer_poll(api, peer, 0, &event);
  assert(test.track_reads == 1u);
  assert(h2_pion_test_opus_send_attempts(peer) == 1u);
  (void)h2_pal_webrtc_peer_poll(api, peer, 0, &event);
  assert(test.track_reads == 1u);
  assert(h2_pion_test_opus_send_attempts(peer) == 2u);
  assert(h2_pion_test_opus_send_payloads_match(peer));
  assert(h2_pal_webrtc_peer_unset_track(api, peer, &track) == H2_PAL_OK);

  s_fail_next_alloc = 1;
  assert(h2_pion_bridge_emit_channel_open((uintptr_t)peer, 99u, "remote", 6u, 1,
                                          2u, 1, 1, 1) == H2_PAL_ERR_NO_MEMORY);
  h2_pal_webrtc_channel_config_t channel_config = {
      .label = {.data = "rpc", .len = 3u}, .ordered = 1, .reliable = 1};
  h2_pal_webrtc_channel_t *channel = NULL;
  assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &channel_config,
                                                &channel) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_LOCAL_SDP);
  assert(event.sdp_type == H2_PAL_WEBRTC_SDP_OFFER);
  assert(event.sdp.data != NULL && event.sdp.len != 0u);
  assert(contains_bytes(event.sdp.data, event.sdp.len, "m=application", 13u));
  h2_pal_webrtc_event_release(&event);

  h2_pal_webrtc_channel_close(api, channel);
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_STATE);
  assert(event.channel_state == H2_PAL_WEBRTC_CHANNEL_CLOSED);
  h2_pal_webrtc_event_release(&event);
  h2_pal_webrtc_peer_close(api, peer);
  h2_pion_destroy(&provider);
  assert(provider == NULL);
  return 0;
}
