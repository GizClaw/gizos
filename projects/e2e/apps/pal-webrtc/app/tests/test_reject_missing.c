#include "h2_pal_webrtc_e2e.h"
#include <stdlib.h>
#include <string.h>
static unsigned calls;
static h2_pal_result_t create(void *u, h2_pal_webrtc_peer_t **p) {
  (void)u;
  (void)p;
  ++calls;
  return H2_PAL_ERR_IO;
}
static h2_pal_result_t configured(void *u, const h2_pal_webrtc_peer_config_t *c,
                                  h2_pal_webrtc_peer_t **p) {
  (void)c;
  return create(u, p);
}
static h2_pal_result_t ice(h2_pal_webrtc_peer_t *p,
                           const h2_pal_webrtc_ice_server_t *s) {
  (void)p;
  (void)s;
  ++calls;
  return H2_PAL_ERR_IO;
}
static h2_pal_result_t offer(h2_pal_webrtc_peer_t *p) {
  (void)p;
  ++calls;
  return H2_PAL_ERR_IO;
}
static h2_pal_result_t sdp(h2_pal_webrtc_peer_t *p, h2_pal_webrtc_sdp_type_t t,
                           h2_pal_webrtc_str_t s) {
  (void)t;
  (void)s;
  return offer(p);
}
static h2_pal_result_t channel(h2_pal_webrtc_peer_t *p,
                               const h2_pal_webrtc_channel_config_t *c,
                               h2_pal_webrtc_channel_t **o) {
  (void)c;
  (void)o;
  return offer(p);
}
static h2_pal_result_t track(h2_pal_webrtc_peer_t *p,
                             h2_pal_webrtc_track_t *t) {
  (void)t;
  return offer(p);
}
static h2_pal_result_t poll(h2_pal_webrtc_peer_t *p, int t,
                            h2_pal_webrtc_event_t *e) {
  (void)t;
  (void)e;
  return offer(p);
}
static h2_pal_result_t opus(h2_pal_webrtc_peer_t *p, const uint8_t *b,
                            size_t n) {
  (void)b;
  (void)n;
  return offer(p);
}
static h2_pal_result_t send(h2_pal_webrtc_channel_t *c, const uint8_t *b,
                            size_t n, int t) {
  (void)c;
  (void)b;
  (void)n;
  (void)t;
  ++calls;
  return H2_PAL_ERR_IO;
}
static void close_channel(h2_pal_webrtc_channel_t *c) {
  (void)c;
  ++calls;
}
static void close_peer(h2_pal_webrtc_peer_t *p) {
  (void)p;
  ++calls;
}
static void *allocate(void *u, size_t n) {
  (void)u;
  return malloc(n);
}
static void *resize(void *u, void *p, size_t n) {
  (void)u;
  return realloc(p, n);
}
static void release(void *u, void *p) {
  (void)u;
  free(p);
}
static h2_pal_result_t clock_ms(void *u, uint64_t *out) {
  (void)u;
  static uint64_t now;
  *out = ++now;
  return 0;
}
static int exchange(void *u, h2_pal_webrtc_str_t s,
                    const h2_pal_webrtc_channel_config_t *negotiated,
                    char *a, size_t n,
                    size_t *o) {
  (void)u;
  (void)s;
  (void)negotiated;
  (void)a;
  (void)n;
  (void)o;
  ++calls;
  return H2_PAL_ERR_IO;
}
static int remote(void *u) {
  (void)u;
  ++calls;
  return H2_PAL_ERR_IO;
}
static h2_pal_result_t sleep_ms(void *user, uint32_t milliseconds) {
  (void)user;
  (void)milliseconds;
  return H2_PAL_OK;
}
int main(void) {
  const h2_pal_mem_vtable_t mv = {allocate, resize, release};
  const h2_pal_mem_api_t memory = {NULL, &mv};
  const h2_pal_time_vtable_t tv = {.get_monotonic_ms = clock_ms,
                                   .sleep_ms = sleep_ms};
  const h2_pal_time_api_t time = {NULL, &tv};
  const h2_pal_webrtc_vtable_t complete = {
      create, ice,  offer, sdp,           channel,    track,     track,
      poll,   opus, send,  close_channel, close_peer, configured};
  for (unsigned missing = 0; missing < 14u; ++missing) {
    h2_pal_webrtc_vtable_t v = complete;
    switch (missing) {
    case 0:
      v.peer_create = NULL;
      break;
    case 1:
      v.peer_add_ice_server = NULL;
      break;
    case 2:
      v.peer_start_offer = NULL;
      break;
    case 3:
      v.peer_set_remote_sdp = NULL;
      break;
    case 4:
      v.peer_create_data_channel = NULL;
      break;
    case 5:
      v.peer_set_track = NULL;
      break;
    case 6:
      v.peer_unset_track = NULL;
      break;
    case 7:
      v.peer_poll = NULL;
      break;
    case 8:
      v.peer_send_opus = NULL;
      break;
    case 9:
      v.channel_send = NULL;
      break;
    case 10:
      v.channel_close = NULL;
      break;
    case 11:
      v.peer_close = NULL;
      break;
    case 12:
      v.peer_create_with_config = NULL;
      break;
    default:
      break;
    }
    const h2_pal_webrtc_api_t api = {NULL, missing == 13 ? NULL : &v};
    h2_runtime_t runtime = {.webrtc = &api, .mem = &memory, .time = &time};
    h2_pal_webrtc_e2e_config_t config = {.runtime = &runtime,
                                         .stun_url = "stun:invalid",
                                         .exchange_offer = exchange,
                                         .close_remote = remote};
    h2_pal_webrtc_e2e_result_t result;
    memset(&result, 0xa5, sizeof(result));
    if (h2_pal_webrtc_e2e_run(&config, &result) == H2_PAL_OK || calls ||
        result.passed || result.failed ||
        result.blocked != H2_PAL_WEBRTC_E2E_CASE_COUNT)
      return 1;
  }
  return 0;
}
