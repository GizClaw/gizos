#include "h2_pion.h"
#include "h2_pion_bridge.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_state {
  const h2_pal_webrtc_api_t *api;
  h2_pion_t **provider;
  int local_sdp_count;
  int channel_closed_count;
  int peer_closed_count;
  int close_on_local_sdp;
  int destroy_on_local_sdp;
  int destroy_on_peer_closed;
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

static void on_peer_state(void *user, h2_pal_webrtc_peer_t *peer,
                          h2_pal_webrtc_peer_state_t state) {
  (void)peer;
  test_state_t *test = user;
  if (state == H2_PAL_WEBRTC_PEER_CLOSED) {
    test->peer_closed_count++;
    if (test->destroy_on_peer_closed) {
      h2_pion_destroy(test->provider);
    }
  }
}

static int contains_bytes(const char *data, size_t len, const char *needle,
                          size_t needle_len) {
  if (needle_len == 0u || needle_len > len) {
    return 0;
  }
  for (size_t offset = 0u; offset <= len - needle_len; ++offset) {
    if (memcmp(data + offset, needle, needle_len) == 0) {
      return 1;
    }
  }
  return 0;
}

static void on_local_sdp(void *user, h2_pal_webrtc_peer_t *peer,
                         h2_pal_webrtc_sdp_type_t type,
                         h2_pal_webrtc_str_t sdp) {
  (void)peer;
  test_state_t *test = user;
  assert(type == H2_PAL_WEBRTC_SDP_OFFER);
  assert(sdp.data != NULL);
  assert(sdp.len != 0u);
  assert(contains_bytes(sdp.data, sdp.len, "m=application", 13u));
  test->local_sdp_count++;
  if (test->close_on_local_sdp) {
    h2_pal_webrtc_peer_close(test->api, peer);
  }
  if (test->destroy_on_local_sdp) {
    h2_pion_destroy(test->provider);
  }
}

static void on_channel_state(void *user, h2_pal_webrtc_peer_t *peer,
                             h2_pal_webrtc_channel_t *channel,
                             const h2_pal_webrtc_channel_info_t *info,
                             h2_pal_webrtc_channel_state_t state) {
  (void)peer;
  test_state_t *test = user;
  assert(channel != NULL);
  assert(info != NULL);
  if (state == H2_PAL_WEBRTC_CHANNEL_CLOSED) {
    test->channel_closed_count++;
    h2_pal_webrtc_channel_close(test->api, channel);
  }
}

int main(void) {
  static const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  h2_pal_mem_api_t mem = {.user = NULL, .vtable = &mem_vtable};
  h2_pion_t *provider = NULL;
  assert(h2_pion_create(NULL, &provider) == H2_PAL_ERR_INVALID_ARG);
  h2_pion_config_t config = {.mem = &mem};
  assert(h2_pion_create(&config, &provider) == H2_PAL_OK);
  assert(provider != NULL);

  const h2_pal_webrtc_api_t *api = h2_pion_webrtc_api(provider);
  assert(api != NULL);
  test_state_t test = {.api = api, .provider = &provider};
  h2_pal_webrtc_callbacks_t callbacks = {
      .user = &test,
      .on_peer_state = on_peer_state,
      .on_local_sdp = on_local_sdp,
      .on_channel_state = on_channel_state,
  };
  h2_pal_webrtc_peer_t *peer = NULL;
  assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);

  s_fail_next_alloc = 1;
  assert(h2_pion_bridge_emit_channel_open(
             (uintptr_t)peer, 99u, "remote", 6u, 1, 2u, 1, 1, 1) ==
         H2_PAL_ERR_NO_MEMORY);

  h2_pal_webrtc_channel_config_t channel_config = {
      .label = {.data = "rpc", .len = 3u},
      .ordered = 1,
      .reliable = 1,
  };
  h2_pal_webrtc_channel_t *channel = NULL;
  assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &channel_config,
                                                &channel) == H2_PAL_OK);
  assert(channel != NULL);
  assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
  assert(test.local_sdp_count == 1);

  h2_pal_webrtc_channel_close(api, channel);
  assert(test.channel_closed_count == 1);
  test.destroy_on_peer_closed = 1;
  h2_pal_webrtc_peer_close(api, peer);
  assert(test.peer_closed_count == 1);
  assert(provider == NULL);

  h2_pion_destroy(&provider);
  assert(provider == NULL);
  h2_pion_destroy(&provider);

  assert(h2_pion_create(&config, &provider) == H2_PAL_OK);
  api = h2_pion_webrtc_api(provider);
  assert(api != NULL);
  test = (test_state_t){
      .api = api,
      .provider = &provider,
      .close_on_local_sdp = 1,
      .destroy_on_peer_closed = 1,
  };
  callbacks.user = &test;
  peer = NULL;
  assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
  channel = NULL;
  assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &channel_config,
                                                &channel) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_ERR_CLOSED);
  assert(test.local_sdp_count == 1);
  assert(test.channel_closed_count == 1);
  assert(test.peer_closed_count == 1);
  assert(provider == NULL);

  assert(h2_pion_create(&config, &provider) == H2_PAL_OK);
  api = h2_pion_webrtc_api(provider);
  assert(api != NULL);
  test = (test_state_t){
      .api = api,
      .provider = &provider,
      .destroy_on_local_sdp = 1,
  };
  callbacks.user = &test;
  peer = NULL;
  assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
  channel = NULL;
  assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &channel_config,
                                                &channel) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_ERR_CLOSED);
  assert(test.local_sdp_count == 1);
  assert(test.channel_closed_count == 1);
  assert(test.peer_closed_count == 1);
  assert(provider == NULL);
  return 0;
}
