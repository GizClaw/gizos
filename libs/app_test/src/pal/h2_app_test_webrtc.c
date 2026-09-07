#include "h2_app_test_webrtc.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
typedef struct peer peer_t;
typedef struct channel {
  peer_t *peer;
  h2_pal_webrtc_channel_t *delegate;
} channel_t;
struct h2_app_test_webrtc {
  h2_pal_webrtc_api_t api;
  const h2_pal_mem_api_t *mem;
  const h2_pal_webrtc_api_t *delegate;
  h2_app_test_webrtc_observe_fn observe;
  void *user;
  atomic_uint peers, events;
};
struct peer {
  h2_app_test_webrtc_t *owner;
  h2_pal_webrtc_peer_t *delegate;
  channel_t channels[H2_APP_TEST_WEBRTC_CHANNELS_MAX];
};
typedef struct event {
  h2_app_test_webrtc_t *owner;
  h2_pal_webrtc_event_t original;
} event_t;
static channel_t *channel_for(peer_t *p, h2_pal_webrtc_channel_t *delegate) {
  if (!delegate)
    return NULL;
  for (size_t i = 0; i < H2_APP_TEST_WEBRTC_CHANNELS_MAX; ++i)
    if (p->channels[i].delegate == delegate)
      return &p->channels[i];
  for (size_t i = 0; i < H2_APP_TEST_WEBRTC_CHANNELS_MAX; ++i)
    if (!p->channels[i].delegate) {
      p->channels[i] = (channel_t){p, delegate};
      return &p->channels[i];
    }
  return NULL;
}
static h2_pal_result_t peer_create(void *user, h2_pal_webrtc_peer_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  h2_app_test_webrtc_t *w = user;
  peer_t *p = h2_pal_mem_alloc(w->mem, sizeof(*p));
  if (!p)
    return H2_PAL_ERR_NO_MEMORY;
  memset(p, 0, sizeof(*p));
  p->owner = w;
  int rc = h2_pal_webrtc_peer_create(w->delegate, &p->delegate);
  if (rc) {
    h2_pal_mem_free(w->mem, p);
    return rc;
  }
  atomic_fetch_add(&w->peers, 1u);
  *out = (h2_pal_webrtc_peer_t *)p;
  return H2_PAL_OK;
}
static h2_pal_result_t
peer_add_ice_server(h2_pal_webrtc_peer_t *peer,
                    const h2_pal_webrtc_ice_server_t *server) {
  peer_t *p = (peer_t *)peer;
  return h2_pal_webrtc_peer_add_ice_server(p->owner->delegate, p->delegate,
                                           server);
}
static h2_pal_result_t peer_start_offer(h2_pal_webrtc_peer_t *peer) {
  peer_t *p = (peer_t *)peer;
  return h2_pal_webrtc_peer_start_offer(p->owner->delegate, p->delegate);
}
static h2_pal_result_t peer_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                                           h2_pal_webrtc_sdp_type_t type,
                                           h2_pal_webrtc_str_t sdp) {
  peer_t *p = (peer_t *)peer;
  return h2_pal_webrtc_peer_set_remote_sdp(p->owner->delegate, p->delegate,
                                           type, sdp);
}
static h2_pal_result_t peer_set_track(h2_pal_webrtc_peer_t *peer,
                                      h2_pal_webrtc_track_t *track) {
  peer_t *p = (peer_t *)peer;
  return h2_pal_webrtc_peer_set_track(p->owner->delegate, p->delegate, track);
}
static h2_pal_result_t peer_unset_track(h2_pal_webrtc_peer_t *peer,
                                        h2_pal_webrtc_track_t *track) {
  peer_t *p = (peer_t *)peer;
  return h2_pal_webrtc_peer_unset_track(p->owner->delegate, p->delegate, track);
}
static h2_pal_result_t peer_send_opus(h2_pal_webrtc_peer_t *peer,
                                      const uint8_t *opus, size_t len) {
  peer_t *p = (peer_t *)peer;
  return h2_pal_webrtc_peer_send_opus(p->owner->delegate, p->delegate, opus,
                                      len);
}
static h2_pal_result_t
peer_create_data_channel(h2_pal_webrtc_peer_t *peer,
                         const h2_pal_webrtc_channel_config_t *config,
                         h2_pal_webrtc_channel_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  peer_t *p = (peer_t *)peer;
  bool room = false;
  for (size_t i = 0; i < H2_APP_TEST_WEBRTC_CHANNELS_MAX; ++i)
    if (!p->channels[i].delegate)
      room = true;
  if (!room)
    return H2_PAL_ERR_NO_SPACE;
  h2_pal_webrtc_channel_t *raw = NULL;
  int rc = h2_pal_webrtc_peer_create_data_channel(p->owner->delegate,
                                                  p->delegate, config, &raw);
  if (!rc)
    *out = (h2_pal_webrtc_channel_t *)channel_for(p, raw);
  return rc;
}
static void event_release(h2_pal_webrtc_event_t *event) {
  event_t *e = event->_private;
  h2_app_test_webrtc_t *w = e->owner;
  h2_pal_webrtc_event_release(&e->original);
  h2_pal_mem_free(w->mem, e);
  atomic_fetch_sub(&w->events, 1u);
  memset(event, 0, sizeof(*event));
}
static h2_pal_result_t peer_poll(h2_pal_webrtc_peer_t *peer, int timeout,
                                 h2_pal_webrtc_event_t *out) {
  peer_t *p = (peer_t *)peer;
  h2_app_test_webrtc_t *w = p->owner;
  /* Reserve ownership before consuming an upstream event. */
  event_t *e = h2_pal_mem_alloc(w->mem, sizeof(*e));
  if (!e)
    return H2_PAL_ERR_NO_MEMORY;
  int rc =
      h2_pal_webrtc_peer_poll(w->delegate, p->delegate, timeout, &e->original);
  if (rc) {
    h2_pal_mem_free(w->mem, e);
    return rc;
  }
  channel_t *c = channel_for(p, e->original.channel);
  if (e->original.channel && !c) {
    h2_pal_webrtc_event_release(&e->original);
    h2_pal_mem_free(w->mem, e);
    return H2_PAL_ERR_NO_SPACE;
  }
  e->owner = w;
  *out = e->original;
  out->peer = e->original.peer ? peer : NULL;
  out->channel = (h2_pal_webrtc_channel_t *)c;
  out->_private = e;
  out->_release = event_release;
  atomic_fetch_add(&w->events, 1u);
  if (w->observe)
    w->observe(w->user, out);
  return H2_PAL_OK;
}
static h2_pal_result_t channel_send(h2_pal_webrtc_channel_t *channel,
                                    const uint8_t *data, size_t len, int text) {
  channel_t *c = (channel_t *)channel;
  return h2_pal_webrtc_channel_send(c->peer->owner->delegate, c->delegate, data,
                                    len, text);
}
static void channel_close(h2_pal_webrtc_channel_t *channel) {
  channel_t *c = (channel_t *)channel;
  h2_pal_webrtc_channel_close(c->peer->owner->delegate, c->delegate);
}
static void peer_close(h2_pal_webrtc_peer_t *peer) {
  peer_t *p = (peer_t *)peer;
  h2_app_test_webrtc_t *w = p->owner;
  h2_pal_webrtc_peer_close(w->delegate, p->delegate);
  h2_pal_mem_free(w->mem, p);
  atomic_fetch_sub(&w->peers, 1u);
}
static const h2_pal_webrtc_vtable_t vtable = {
    .peer_create = peer_create,
    .peer_add_ice_server = peer_add_ice_server,
    .peer_start_offer = peer_start_offer,
    .peer_set_remote_sdp = peer_set_remote_sdp,
    .peer_create_data_channel = peer_create_data_channel,
    .peer_set_track = peer_set_track,
    .peer_unset_track = peer_unset_track,
    .peer_poll = peer_poll,
    .peer_send_opus = peer_send_opus,
    .channel_send = channel_send,
    .channel_close = channel_close,
    .peer_close = peer_close,
};
h2_pal_result_t h2_app_test_webrtc_create(const h2_pal_mem_api_t *mem,
                                          const h2_pal_webrtc_api_t *delegate,
                                          h2_app_test_webrtc_observe_fn observe,
                                          void *user,
                                          h2_app_test_webrtc_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (!mem || !mem->vtable || !mem->vtable->alloc || !mem->vtable->free ||
      !delegate || !delegate->vtable)
    return H2_PAL_ERR_INVALID_ARG;
  h2_app_test_webrtc_t *w = h2_pal_mem_alloc(mem, sizeof(*w));
  if (!w)
    return H2_PAL_ERR_NO_MEMORY;
  memset(w, 0, sizeof(*w));
  w->mem = mem;
  w->delegate = delegate;
  w->observe = observe;
  w->user = user;
  w->api = (h2_pal_webrtc_api_t){w, &vtable};
  atomic_init(&w->peers, 0u);
  atomic_init(&w->events, 0u);
  *out = w;
  return H2_PAL_OK;
}
const h2_pal_webrtc_api_t *h2_app_test_webrtc_api(h2_app_test_webrtc_t *w) {
  return w ? &w->api : NULL;
}
h2_pal_result_t h2_app_test_webrtc_destroy(h2_app_test_webrtc_t *w) {
  if (!w)
    return H2_PAL_OK;
  if (atomic_load(&w->peers) || atomic_load(&w->events))
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_mem_free(w->mem, w);
  return H2_PAL_OK;
}
