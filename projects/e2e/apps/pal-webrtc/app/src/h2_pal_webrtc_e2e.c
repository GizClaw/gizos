#include "h2_pal_webrtc_e2e.h"
#include "h2_atomic.h"
#include <string.h>

static const char *const case_ids[] = {
#define H2_PAL_WEBRTC_CASE(symbol, id) id,
#include "h2_pal_webrtc_cases.inc"
#undef H2_PAL_WEBRTC_CASE
};
static const uint8_t opus_tag[] = {0xf8, 'H', '2', 'P', 'A', 'L',
                                   'R',  'T', 'C', 1,   2,   3};
typedef struct allocator {
  const h2_pal_mem_api_t *base;
  h2_atomic_uint_t live, calls;
  h2_atomic_int_t fail;
} allocator_t;
typedef struct track_state {
  h2_atomic_uint_t reads, writes, attempts, budget, detached, late, block;
  uint8_t retry[H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE];
  size_t retry_len;
  int retry_mismatch;
} track_state_t;
typedef struct state {
  const h2_pal_webrtc_e2e_config_t *config;
  const h2_pal_webrtc_api_t *api;
  h2_pal_webrtc_peer_t *peer;
  h2_pal_webrtc_channel_t *channels[4];
  unsigned opened;
  h2_pal_webrtc_peer_state_t peer_state;
  h2_pal_webrtc_event_t held;
  h2_pal_mem_api_t memory;
  allocator_t allocator;
  track_state_t track_state;
  h2_pal_webrtc_track_t track;
  int track_bound;
  int observed_error, authentication_evidence;
  unsigned line;
  uint8_t message[2048];
  char answer[16384];
} state_t;

static int allocation_allowed(allocator_t *a) {
  int remaining = h2_atomic_load(&a->fail);
  while (remaining > 1) {
    if (h2_atomic_compare_exchange_strong(&a->fail, &remaining, remaining - 1))
      return 1;
  }
  return remaining == 0;
}
static void *allocate(void *user, size_t len) {
  allocator_t *a = user;
  h2_atomic_fetch_add(&a->calls, 1u);
  if (!allocation_allowed(a))
    return NULL;
  void *p = h2_pal_mem_alloc(a->base, len);
  if (p != NULL)
    h2_atomic_fetch_add(&a->live, 1u);
  return p;
}
static void release(void *user, void *ptr) {
  allocator_t *a = user;
  if (ptr != NULL) {
    h2_pal_mem_free(a->base, ptr);
    h2_atomic_fetch_sub(&a->live, 1u);
  }
}
static void *resize(void *user, void *ptr, size_t len) {
  allocator_t *a = user;
  if (ptr == NULL)
    return allocate(user, len);
  if (len == 0u) {
    release(user, ptr);
    return NULL;
  }
  h2_atomic_fetch_add(&a->calls, 1u);
  if (!allocation_allowed(a))
    return NULL;
  return h2_pal_mem_realloc(a->base, ptr, len);
}
static const h2_pal_mem_vtable_t memory_vtable = {allocate, resize, release};
static h2_pal_result_t track_read(void *user, uint8_t *opus, size_t capacity,
                                  size_t *out_len) {
  track_state_t *t = user;
  if (h2_atomic_load(&t->detached))
    h2_atomic_fetch_add(&t->late, 1u);
  h2_atomic_fetch_add(&t->reads, 1u);
  *out_len = 0u;
  if (!h2_atomic_load(&t->budget))
    return H2_PAL_ERR_WOULD_BLOCK;
  if (capacity < sizeof(opus_tag))
    return H2_PAL_ERR_NO_SPACE;
  memcpy(opus, opus_tag, sizeof(opus_tag));
  *out_len = sizeof(opus_tag);
  h2_atomic_fetch_sub(&t->budget, 1u);
  return H2_PAL_OK;
}
static h2_pal_result_t track_write(void *user, const uint8_t *opus,
                                   size_t len) {
  track_state_t *t = user;
  if (h2_atomic_load(&t->detached))
    h2_atomic_fetch_add(&t->late, 1u);
  if (len == 0u)
    return H2_PAL_OK; /* A loss marker is not an echoed packet. */
  if (len != sizeof(opus_tag) && len != H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE)
    return H2_PAL_ERR_FORMAT;
  for (size_t i = 0; i < len; ++i)
    if (opus[i] != opus_tag[i % sizeof(opus_tag)])
      return H2_PAL_ERR_FORMAT;
  h2_atomic_fetch_add(&t->attempts, 1u);
  if (t->retry_len != 0u &&
      (t->retry_len != len || memcmp(t->retry, opus, len)))
    t->retry_mismatch = 1;
  if (h2_atomic_load(&t->block)) {
    memcpy(t->retry, opus, len);
    t->retry_len = len;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  t->retry_len = 0u;
  h2_atomic_fetch_add(&t->writes, 1u);
  return H2_PAL_OK;
}
static const h2_pal_webrtc_track_vtable_t track_vtable = {track_read,
                                                          track_write};
static uint64_t now(state_t *s) {
  uint64_t value = 0u;
  (void)h2_pal_time_get_monotonic_ms(s->config->runtime->time, &value);
  return value;
}
static void pump(state_t *s) {
  if (s->config->pump != NULL)
    s->config->pump(s->config->pump_user);
}
static int poll_event(state_t *s, int timeout, h2_pal_webrtc_event_t *event) {
  pump(s);
  int rc = h2_pal_webrtc_peer_poll(s->api, s->peer, timeout, event);
  if (rc == H2_PAL_OK && event->kind == H2_PAL_WEBRTC_EVENT_ERROR)
    return event->error != H2_PAL_OK ? event->error : H2_PAL_ERR_IO;
  if (rc == H2_PAL_OK && event->kind == H2_PAL_WEBRTC_EVENT_PEER_STATE)
    s->peer_state = event->peer_state;
  return rc;
}
static uint32_t connection_timeout(state_t *s) {
  return s->config->connection_timeout_ms != 0u
             ? s->config->connection_timeout_ms : 20000u;
}
static int wait_kind_for(state_t *s, h2_pal_webrtc_event_kind_t kind,
                         h2_pal_webrtc_event_t *out, uint32_t timeout_ms) {
  const uint64_t deadline = now(s) + timeout_ms;
  while (now(s) < deadline) {
    int rc = poll_event(s, 50, out);
    if (rc == H2_PAL_OK) {
      if (out->kind == kind)
        return H2_PAL_OK;
      h2_pal_webrtc_event_release(out);
    } else if (rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_WOULD_BLOCK)
      return rc;
  }
  return H2_PAL_ERR_TIMEOUT;
}
static int wait_kind(state_t *s, h2_pal_webrtc_event_kind_t kind,
                     h2_pal_webrtc_event_t *out) {
  return wait_kind_for(s, kind, out, 20000u);
}
static int send(state_t *s, unsigned index, const uint8_t *data, size_t len,
                int text) {
  const uint64_t deadline = now(s) + 10000u;
  for (;;) {
    int rc =
        h2_pal_webrtc_channel_send(s->api, s->channels[index], data, len, text);
    if (rc != H2_PAL_ERR_WOULD_BLOCK)
      return rc;
    if (now(s) >= deadline)
      return H2_PAL_ERR_TIMEOUT;
    h2_pal_webrtc_event_t e = {0};
    rc = poll_event(s, 20, &e);
    h2_pal_webrtc_event_release(&e);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT)
      return rc;
  }
}
static int echo(state_t *s, unsigned index, const uint8_t *data, size_t len,
                int text, int mutate, int retain) {
  uint8_t sent[2048];
  if (len > sizeof(sent))
    return H2_PAL_ERR_INVALID_ARG;
  if (len)
    memcpy(sent, data, len);
  int rc = send(s, index, len ? sent : NULL, len, text);
  if (rc != H2_PAL_OK)
    return rc;
  if (mutate)
    memset(sent, 0xa5, sizeof(sent));
  h2_pal_webrtc_event_t event = {0};
  rc = wait_kind(s, H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE, &event);
  const char *prefix = text ? "server-echo-text:" : "server-echo-binary:";
  const size_t prefix_len = strlen(prefix);
  if (rc == H2_PAL_OK &&
      (event.channel != s->channels[index] || event.is_text != text ||
       event.data_len != prefix_len + len ||
       memcmp(event.data, prefix, prefix_len) ||
       (len && memcmp(event.data + prefix_len, data, len))))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK && retain)
    s->held = event;
  else
    h2_pal_webrtc_event_release(&event);
  return rc;
}
static int wait_writes(state_t *s, unsigned expected) {
  const uint64_t deadline = now(s) + 10000u;
  while (h2_atomic_load(&s->track_state.writes) < expected &&
         now(s) < deadline) {
    h2_pal_webrtc_event_t event = {0};
    int rc = poll_event(s, 20, &event);
    h2_pal_webrtc_event_release(&event);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT)
      return rc;
  }
  return h2_atomic_load(&s->track_state.writes) >= expected
             ? H2_PAL_OK
             : H2_PAL_ERR_TIMEOUT;
}
static int opus_send_packet(state_t *s, const uint8_t *packet, size_t length) {
  const uint64_t deadline = now(s) + 10000u;
  do {
    int rc = h2_pal_webrtc_peer_send_opus(s->api, s->peer, packet, length);
    if (rc != H2_PAL_ERR_WOULD_BLOCK)
      return rc;
    pump(s);
    (void)h2_pal_time_sleep_ms(s->config->runtime->time, 2u);
  } while (now(s) < deadline);
  return H2_PAL_ERR_TIMEOUT;
}
static int opus_send(state_t *s) {
  return opus_send_packet(s, opus_tag, sizeof(opus_tag));
}
static int wait_released(state_t *s) {
  uint64_t deadline = now(s) + 5000u;
  while (h2_atomic_load(&s->allocator.live) && now(s) < deadline) {
    pump(s);
    (void)h2_pal_time_sleep_ms(s->config->runtime->time, 2u);
  }
  return h2_atomic_load(&s->allocator.live) == 0u ? H2_PAL_OK : H2_PAL_ERR_BUSY;
}
static int complete(const h2_pal_webrtc_vtable_t *v) {
  return v && v->peer_create && v->peer_create_with_config &&
         v->peer_add_ice_server && v->peer_start_offer &&
         v->peer_set_remote_sdp && v->peer_create_data_channel &&
         v->peer_set_track && v->peer_unset_track && v->peer_poll &&
         v->peer_send_opus && v->channel_send && v->channel_close &&
         v->peer_close;
}
#define REQUIRE(x)                                                             \
  do {                                                                         \
    if (!(x)) {                                                                \
      s->line = __LINE__;                                                      \
      return H2_PAL_ERR_INVALID_STATE;                                         \
    }                                                                          \
  } while (0)
#define CALL(x)                                                                \
  do {                                                                         \
    int rc_ = (x);                                                             \
    if (rc_ != H2_PAL_OK) {                                                    \
      s->line = __LINE__;                                                      \
      return rc_;                                                              \
    }                                                                          \
  } while (0)
static int run_case(state_t *s, unsigned id) {
  const h2_pal_webrtc_api_t *a = s->api;
  h2_pal_webrtc_event_t event = {0};
  h2_pal_webrtc_peer_t *peer = NULL;
  const h2_pal_webrtc_peer_config_t custom = {.allocator = &s->memory};
  switch (id) {
  case H2_PAL_WEBRTC_E2E_WRAPPER_ARGUMENTS:
    REQUIRE(h2_pal_webrtc_peer_create(NULL, &peer) == H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_peer_create(a, NULL) == H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_peer_create_with_config(NULL, NULL, &peer) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_peer_start_offer(a, NULL) == H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_peer_poll(a, NULL, 0, &event) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_channel_send(a, NULL, NULL, 0, 0) ==
            H2_PAL_ERR_INVALID_ARG);
    h2_pal_webrtc_channel_close(a, NULL);
    h2_pal_webrtc_peer_close(a, NULL);
    h2_pal_webrtc_event_release(NULL);
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_MALFORMED_SDP: {
    CALL(h2_pal_webrtc_peer_create(a, &s->peer));
    int invalid =
        h2_pal_webrtc_peer_set_remote_sdp(a, s->peer, H2_PAL_WEBRTC_SDP_ANSWER,
                                          (h2_pal_webrtc_str_t){"bad-sdp", 7u});
    h2_pal_webrtc_peer_close(a, s->peer);
    s->peer = NULL;
    REQUIRE(invalid != H2_PAL_OK);
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_DEFAULT_LIFETIME:
    CALL(h2_pal_webrtc_peer_create(a, &s->peer));
    REQUIRE(s->peer != NULL);
    h2_pal_webrtc_peer_close(a, s->peer);
    s->peer = NULL;
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_LEGACY_CREATE_FALLBACK: {
    h2_pal_webrtc_vtable_t legacy = *a->vtable;
    legacy.peer_create_with_config = NULL;
    const h2_pal_webrtc_api_t api = {a->user, &legacy};
    REQUIRE(h2_pal_webrtc_peer_create_with_config(&api, &custom, &peer) ==
            H2_PAL_ERR_UNSUPPORTED);
    CALL(h2_pal_webrtc_peer_create_with_config(&api, NULL, &s->peer));
    h2_pal_webrtc_peer_close(&api, s->peer);
    s->peer = NULL;
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_ALLOCATOR_FAILURE: {
    for (int allowed = 0; allowed < 4; ++allowed) {
      h2_atomic_store(&s->allocator.fail, allowed + 1);
      int rc = h2_pal_webrtc_peer_create_with_config(a, &custom, &s->peer);
      h2_atomic_store(&s->allocator.fail, 0);
      if (rc == H2_PAL_OK) {
        REQUIRE(allowed > 0 && s->peer != NULL);
        h2_pal_webrtc_peer_close(a, s->peer);
        s->peer = NULL;
      } else
        REQUIRE(rc == H2_PAL_ERR_NO_MEMORY && s->peer == NULL);
      CALL(wait_released(s));
    }
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_ALLOCATOR_OWNERSHIP:
    CALL(h2_pal_webrtc_peer_create_with_config(a, &custom, &s->peer));
    REQUIRE(h2_atomic_load(&s->allocator.live) > 0u);
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_POLL_NONBLOCKING: {
    int rc;
    do {
      rc = h2_pal_webrtc_peer_poll(a, s->peer, 0, &event);
      h2_pal_webrtc_event_release(&event);
    } while (rc == H2_PAL_OK);
    REQUIRE(rc == H2_PAL_ERR_WOULD_BLOCK);
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_POLL_TIMEOUT: {
    uint64_t start = now(s);
    REQUIRE(h2_pal_webrtc_peer_poll(a, s->peer, 25, &event) ==
            H2_PAL_ERR_TIMEOUT);
    REQUIRE(now(s) - start >= 20u && now(s) - start < 2000u);
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_POLL_NEGATIVE:
    REQUIRE(h2_pal_webrtc_peer_poll(a, s->peer, -1, &event) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_peer_poll(a, s->peer, 0, NULL) ==
            H2_PAL_ERR_INVALID_ARG);
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_ICE_ARGUMENTS: {
    h2_pal_webrtc_ice_server_t ice = {0};
    REQUIRE(h2_pal_webrtc_peer_add_ice_server(a, s->peer, NULL) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_peer_add_ice_server(a, s->peer, &ice) ==
            H2_PAL_ERR_INVALID_ARG);
    ice.url = (h2_pal_webrtc_str_t){"stun:localhost:1", 16};
    ice.username.len = 1;
    REQUIRE(h2_pal_webrtc_peer_add_ice_server(a, s->peer, &ice) ==
            H2_PAL_ERR_INVALID_ARG);
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_ICE_BORROWED_INPUT: {
    char url[256];
    size_t len = strlen(s->config->stun_url);
    REQUIRE(len < sizeof(url));
    memcpy(url, s->config->stun_url, len + 1u);
    const h2_pal_webrtc_ice_server_t ice = {.url = {url, len}};
    CALL(h2_pal_webrtc_peer_add_ice_server(a, s->peer, &ice));
    memset(url, 'X', sizeof(url));
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_TRACK_ARGUMENTS: {
    h2_pal_webrtc_track_t empty = {0};
    REQUIRE(h2_pal_webrtc_peer_set_track(a, s->peer, NULL) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_peer_set_track(a, s->peer, &empty) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_peer_unset_track(a, s->peer, NULL) ==
            H2_PAL_ERR_INVALID_ARG);
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_TRACK_BIND:
    CALL(h2_pal_webrtc_peer_set_track(a, s->peer, &s->track));
    s->track_bound = 1;
    REQUIRE(h2_pal_webrtc_peer_set_track(a, s->peer, &s->track) != H2_PAL_OK);
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_CHANNEL_ARGUMENTS: {
    h2_pal_webrtc_channel_config_t c = {0};
    h2_pal_webrtc_channel_t *channel = NULL;
    REQUIRE(h2_pal_webrtc_peer_create_data_channel(
                a, s->peer, NULL, &channel) == H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_peer_create_data_channel(a, s->peer, &c, &channel) ==
            H2_PAL_ERR_INVALID_ARG);
    c.label = (h2_pal_webrtc_str_t){"x", 1};
    REQUIRE(h2_pal_webrtc_peer_create_data_channel(a, s->peer, &c, NULL) ==
            H2_PAL_ERR_INVALID_ARG);
    c.negotiated = 1;
    REQUIRE(h2_pal_webrtc_peer_create_data_channel(a, s->peer, &c, &channel) ==
            H2_PAL_ERR_INVALID_ARG);
    c.has_stream_id = 1;
    c.stream_id = UINT16_MAX;
    REQUIRE(h2_pal_webrtc_peer_create_data_channel(a, s->peer, &c, &channel) ==
            H2_PAL_ERR_INVALID_ARG);
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_CHANNEL_CONFIGURATIONS:
    for (unsigned i = 0; i < 4u; ++i) {
      char label[] = "pal/0";
      label[4] = (char)('0' + i);
      const h2_pal_webrtc_channel_config_t c = {.label = {label, 5u},
                                                .ordered = !(i & 1u),
                                                .reliable = i == 0u || i == 3u};
      CALL(h2_pal_webrtc_peer_create_data_channel(a, s->peer, &c,
                                                  &s->channels[i]));
      memset(label, 'X', sizeof(label));
    }
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_SEND_BEFORE_CONNECTED:
    REQUIRE(h2_pal_webrtc_channel_send(a, s->channels[0], opus_tag,
                                       sizeof(opus_tag), 0) != H2_PAL_OK);
    REQUIRE(h2_pal_webrtc_peer_send_opus(a, s->peer, opus_tag,
                                         sizeof(opus_tag)) != H2_PAL_OK);
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_OFFER_OWNED_SDP:
    CALL(h2_pal_webrtc_peer_start_offer(a, s->peer));
    CALL(wait_kind(s, H2_PAL_WEBRTC_EVENT_LOCAL_SDP, &s->held));
    REQUIRE(s->held.sdp_type == H2_PAL_WEBRTC_SDP_OFFER &&
            s->held.sdp.len > 0u && s->held._release != NULL);
    REQUIRE(s->held.sdp.len >= 3u && !memcmp(s->held.sdp.data, "v=0", 3u));
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_REMOTE_SDP_ARGUMENTS:
    REQUIRE(h2_pal_webrtc_peer_set_remote_sdp(
                a, s->peer, H2_PAL_WEBRTC_SDP_ANSWER,
                (h2_pal_webrtc_str_t){NULL, 1}) == H2_PAL_ERR_INVALID_ARG);
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_PION_NEGOTIATION: {
    size_t len = 0u;
    CALL(s->config->exchange_offer(s->config->fixture_user, s->held.sdp, NULL,
                                   s->answer, sizeof(s->answer), &len));
    REQUIRE(len && len < sizeof(s->answer));
    h2_pal_webrtc_event_release(&s->held);
    CALL(h2_pal_webrtc_peer_set_remote_sdp(
        a, s->peer, H2_PAL_WEBRTC_SDP_ANSWER,
        (h2_pal_webrtc_str_t){s->answer, len}));
    memset(s->answer, 'X', len);
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_CHANNEL_METADATA: {
    uint64_t deadline = now(s) + connection_timeout(s);
    while (
        (s->opened != 15u || s->peer_state != H2_PAL_WEBRTC_PEER_CONNECTED) &&
        now(s) < deadline) {
      int rc = poll_event(s, 50, &event);
      if (rc == H2_PAL_OK && event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_STATE &&
          event.channel_state == H2_PAL_WEBRTC_CHANNEL_OPEN) {
        int valid = 0;
        for (unsigned i = 0; i < 4u; ++i)
          if (event.channel == s->channels[i]) {
            valid = event.channel_info.label.len == 5u &&
                    !memcmp(event.channel_info.label.data, "pal/", 4u) &&
                    event.channel_info.label.data[4] == (char)('0' + i) &&
                    event.channel_info.ordered == !(i & 1u) &&
                    event.channel_info.reliable == (i == 0u || i == 3u) &&
                    event.channel_info.has_stream_id;
            s->opened |= 1u << i;
          }
        h2_pal_webrtc_event_release(&event);
        REQUIRE(valid);
      } else
        h2_pal_webrtc_event_release(&event);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT)
        return rc;
    }
    REQUIRE(s->opened == 15u && s->peer_state == H2_PAL_WEBRTC_PEER_CONNECTED);
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_BINARY_ROUNDTRIP:
    return echo(s, 0, (const uint8_t[]){0, 1, 255, 0, 128}, 5u, 0, 0, 0);
  case H2_PAL_WEBRTC_E2E_TEXT_ROUNDTRIP:
    return echo(s, 0, (const uint8_t *)"hello \xe4\xb8\xad\xe6\x96\x87", 12u, 1,
                0, 0);
  case H2_PAL_WEBRTC_E2E_EMPTY_BINARY:
    return echo(s, 0, NULL, 0, 0, 0, 0);
  case H2_PAL_WEBRTC_E2E_EMPTY_TEXT:
    return echo(s, 0, NULL, 0, 1, 0, 0);
  case H2_PAL_WEBRTC_E2E_UNORDERED_ROUNDTRIP:
    return echo(s, 1, opus_tag, sizeof(opus_tag), 0, 0, 0);
  case H2_PAL_WEBRTC_E2E_ORDERED_UNRELIABLE:
    return echo(s, 2, opus_tag, sizeof(opus_tag), 0, 0, 0);
  case H2_PAL_WEBRTC_E2E_UNORDERED_RELIABLE:
    return echo(s, 3, opus_tag, sizeof(opus_tag), 0, 0, 0);
  case H2_PAL_WEBRTC_E2E_MESSAGE_BOUNDARIES:
    for (size_t i = 0; i < sizeof(s->message); ++i)
      s->message[i] = (uint8_t)(i * 17u);
    CALL(echo(s, 0, s->message, 1u, 0, 0, 0));
    CALL(echo(s, 0, s->message, 1024u, 0, 0, 0));
    return echo(s, 0, s->message, sizeof(s->message), 0, 0, 0);
  case H2_PAL_WEBRTC_E2E_SEND_BORROWED_INPUT:
    return echo(s, 0, s->message, 128u, 0, 1, 0);
  case H2_PAL_WEBRTC_E2E_OPUS_ARGUMENTS: {
    uint8_t packet[H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE + 1u] = {0};
    REQUIRE(h2_pal_webrtc_peer_send_opus(a, s->peer, NULL, 1u) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_peer_send_opus(a, s->peer, packet, 0u) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_peer_send_opus(a, s->peer, packet, sizeof(packet)) ==
            H2_PAL_ERR_INVALID_ARG);
    REQUIRE(h2_pal_webrtc_channel_send(a, s->channels[0], NULL, 1u, 0) ==
            H2_PAL_ERR_INVALID_ARG);
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_OPUS_DIRECT_ROUNDTRIP: {
    CALL(opus_send(s));
    CALL(wait_writes(s, 1u));
    uint8_t maximum[H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE];
    for (size_t i = 0; i < sizeof(maximum); ++i)
      maximum[i] = opus_tag[i % sizeof(opus_tag)];
    CALL(opus_send_packet(s, maximum, sizeof(maximum)));
    return wait_writes(s, 2u);
  }
  case H2_PAL_WEBRTC_E2E_OPUS_BACKPRESSURE: {
    /* A notification left by an earlier send cannot prove this backpressure
     * transition. Drain it before filling the current send queue. */
    int drained = H2_PAL_OK;
    for (unsigned i = 0; i < 256u && drained == H2_PAL_OK; ++i) {
      drained = poll_event(s, 0, &event);
      h2_pal_webrtc_event_release(&event);
    }
    REQUIRE(drained == H2_PAL_ERR_WOULD_BLOCK);
    unsigned before = h2_atomic_load(&s->track_state.writes), sent = 0u;
    int rc = H2_PAL_OK;
    while (sent < 128u && rc == H2_PAL_OK) {
      rc = h2_pal_webrtc_peer_send_opus(a, s->peer, opus_tag, sizeof(opus_tag));
      if (rc == H2_PAL_OK)
        ++sent;
    }
    REQUIRE(rc == H2_PAL_ERR_WOULD_BLOCK && sent > 0u);
    CALL(wait_kind(s, H2_PAL_WEBRTC_EVENT_WRITABLE, &s->held));
    h2_pal_webrtc_event_release(&s->held);
    CALL(opus_send(s));
    return wait_writes(s, before + sent + 1u);
  }
  case H2_PAL_WEBRTC_E2E_TRACK_READ_ROUNDTRIP: {
    unsigned before = h2_atomic_load(&s->track_state.writes);
    h2_atomic_store(&s->track_state.budget, 3u);
    return wait_writes(s, before + 3u);
  }
  case H2_PAL_WEBRTC_E2E_TRACK_WRITE_RETRY: {
    unsigned before = h2_atomic_load(&s->track_state.attempts);
    unsigned written = h2_atomic_load(&s->track_state.writes);
    h2_atomic_store(&s->track_state.block, 1u);
    CALL(opus_send(s));
    uint64_t deadline = now(s) + 10000u;
    while (h2_atomic_load(&s->track_state.attempts) < before + 2u &&
           now(s) < deadline) {
      pump(s);
      (void)h2_pal_time_sleep_ms(s->config->runtime->time, 5u);
    }
    int retried = h2_atomic_load(&s->track_state.attempts) >= before + 2u;
    h2_atomic_store(&s->track_state.block, 0u);
    REQUIRE(retried);
    return wait_writes(s, written + 1u);
  }
  case H2_PAL_WEBRTC_E2E_TRACK_UNSET_QUIESCENT:
    CALL(h2_pal_webrtc_peer_unset_track(a, s->peer, &s->track));
    s->track_bound = 0;
    h2_atomic_store(&s->track_state.detached, 1u);
    for (unsigned i = 0; i < 10u; ++i) {
      pump(s);
      (void)h2_pal_time_sleep_ms(s->config->runtime->time, 10u);
    }
    REQUIRE(!h2_atomic_load(&s->track_state.late) &&
            !s->track_state.retry_mismatch);
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_DETACHED_OPUS_EVENT:
    CALL(opus_send(s));
    CALL(wait_kind(s, H2_PAL_WEBRTC_EVENT_OPUS_FRAME, &s->held));
    REQUIRE(s->held.data_len == sizeof(opus_tag) &&
            !memcmp(s->held.data, opus_tag, sizeof(opus_tag)));
    h2_pal_webrtc_event_release(&s->held);
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_CHANNEL_CLOSE_EVENT: {
    h2_pal_webrtc_channel_t *identity = s->channels[2];
    h2_pal_webrtc_channel_close(a, identity);
    s->channels[2] = NULL;
    CALL(wait_kind(s, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE, &s->held));
    REQUIRE(s->held.channel == identity &&
            s->held.channel_state == H2_PAL_WEBRTC_CHANNEL_CLOSED);
    REQUIRE(s->held.channel_info.label.len == 5 &&
            !memcmp(s->held.channel_info.label.data, "pal/2", 5));
    h2_pal_webrtc_event_release(&s->held);
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_REMOTE_CLOSE: {
    CALL(echo(s, 0, opus_tag, sizeof(opus_tag), 0, 0, 1));
    CALL(s->config->close_remote(s->config->fixture_user));
    uint64_t deadline = now(s) + 20000u;
    while (s->peer_state != H2_PAL_WEBRTC_PEER_DISCONNECTED &&
           s->peer_state != H2_PAL_WEBRTC_PEER_FAILED &&
           s->peer_state != H2_PAL_WEBRTC_PEER_CLOSED && now(s) < deadline) {
      int rc = poll_event(s, 50, &event);
      h2_pal_webrtc_event_release(&event);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
          rc != H2_PAL_ERR_CLOSED)
        return rc;
    }
    REQUIRE(s->peer_state == H2_PAL_WEBRTC_PEER_DISCONNECTED ||
            s->peer_state == H2_PAL_WEBRTC_PEER_FAILED ||
            s->peer_state == H2_PAL_WEBRTC_PEER_CLOSED);
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_OWNED_EVENT_AFTER_CLOSE:
    h2_pal_webrtc_peer_close(a, s->peer);
    s->peer = NULL;
    memset(s->channels, 0, sizeof(s->channels));
    REQUIRE(s->held.data_len ==
            strlen("server-echo-binary:") + sizeof(opus_tag));
    REQUIRE(!memcmp(s->held.data + strlen("server-echo-binary:"), opus_tag,
                    sizeof(opus_tag)));
    REQUIRE(s->held.channel_info.label.len == 5u &&
            !memcmp(s->held.channel_info.label.data, "pal/0", 5u));
    REQUIRE(h2_atomic_load(&s->allocator.live) > 0u);
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_EVENT_RELEASE_IDEMPOTENT:
    h2_pal_webrtc_event_release(&s->held);
    h2_pal_webrtc_event_release(&s->held);
    REQUIRE(s->held._release == NULL && s->held.data == NULL);
    CALL(wait_released(s));
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_RESOURCE_CHURN:
    for (unsigned i = 0; i < 8u; ++i) {
      CALL(h2_pal_webrtc_peer_create_with_config(a, &custom, &s->peer));
      h2_pal_webrtc_peer_close(a, s->peer);
      s->peer = NULL;
      CALL(wait_released(s));
    }
    return H2_PAL_OK;
  case H2_PAL_WEBRTC_E2E_FINGERPRINT_REJECTED: {
    CALL(h2_pal_webrtc_peer_create_with_config(a, &custom, &s->peer));
    const h2_pal_webrtc_ice_server_t ice = {
        .url = {s->config->stun_url, strlen(s->config->stun_url)}};
    CALL(h2_pal_webrtc_peer_add_ice_server(a, s->peer, &ice));
    const h2_pal_webrtc_channel_config_t c = {
        .label = {"pal/auth", 8u}, .ordered = 1, .reliable = 1};
    CALL(h2_pal_webrtc_peer_create_data_channel(a, s->peer, &c,
                                                &s->channels[0]));
    CALL(h2_pal_webrtc_peer_start_offer(a, s->peer));
    CALL(wait_kind(s, H2_PAL_WEBRTC_EVENT_LOCAL_SDP, &s->held));
    size_t len = 0u;
    CALL(s->config->exchange_offer(s->config->fixture_user, s->held.sdp, NULL,
                                   s->answer, sizeof(s->answer), &len));
    h2_pal_webrtc_event_release(&s->held);
    REQUIRE(len < sizeof(s->answer));
    s->answer[len] = '\0';
    char *fingerprint = strstr(s->answer, "a=fingerprint:sha-256 ");
    REQUIRE(fingerprint != NULL);
    fingerprint += strlen("a=fingerprint:sha-256 ");
    REQUIRE(fingerprint < s->answer + len);
    *fingerprint = *fingerprint == '0' ? '1' : '0';
    s->peer_state = H2_PAL_WEBRTC_PEER_NEW;
    int rc = h2_pal_webrtc_peer_set_remote_sdp(
        a, s->peer, H2_PAL_WEBRTC_SDP_ANSWER,
        (h2_pal_webrtc_str_t){s->answer, len});
    /* This is valid signaling with a deliberately wrong digest. Rejecting
     * malformed SDP or failing to connect is not proof of authentication. */
    s->observed_error = rc;
    REQUIRE(rc == H2_PAL_OK);
    int rejected = 0, terminal = 0;
    uint64_t deadline = now(s) + connection_timeout(s);
    while (!rejected && now(s) < deadline) {
      rc = h2_pal_webrtc_peer_poll(a, s->peer, 50, &event);
      pump(s);
      int connected = rc == H2_PAL_OK &&
                      ((event.kind == H2_PAL_WEBRTC_EVENT_PEER_STATE &&
                        event.peer_state == H2_PAL_WEBRTC_PEER_CONNECTED) ||
                       (event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_STATE &&
                        event.channel_state == H2_PAL_WEBRTC_CHANNEL_OPEN));
      int error = rc == H2_PAL_OK && event.kind == H2_PAL_WEBRTC_EVENT_ERROR
                      ? event.error : rc;
      terminal |= rc == H2_PAL_OK && event.kind == H2_PAL_WEBRTC_EVENT_PEER_STATE &&
                  event.peer_state == H2_PAL_WEBRTC_PEER_FAILED;
      if (error == H2_PAL_ERR_TLS_VERIFY) {
        s->observed_error = error;
        s->authentication_evidence = 1;
        rejected = 1;
      } else if (error == H2_PAL_ERR_IO) {
        if (!s->config->authentication_witness) {
          s->observed_error = error;
          h2_pal_webrtc_event_release(&event);
          return error;
        }
        terminal = 1;
      } else if (error != H2_PAL_OK && error != H2_PAL_ERR_TIMEOUT &&
                 error != H2_PAL_ERR_WOULD_BLOCK) {
        s->observed_error = error;
        h2_pal_webrtc_event_release(&event);
        return error;
      }
      if (rc == H2_PAL_OK && event.kind == H2_PAL_WEBRTC_EVENT_ERROR &&
          event.error == H2_PAL_ERR_TIMEOUT) {
        s->observed_error = event.error;
        h2_pal_webrtc_event_release(&event);
        return H2_PAL_ERR_TIMEOUT;
      }
      h2_pal_webrtc_event_release(&event);
      REQUIRE(!connected);
      if (!rejected && terminal && s->config->authentication_witness) {
        int witnessed = s->config->authentication_witness(s->config->fixture_user);
        if (witnessed == H2_PAL_ERR_TLS_VERIFY) {
          s->observed_error = witnessed;
          s->authentication_evidence = 2;
          rejected = 1;
        } else if (witnessed != H2_PAL_ERR_WOULD_BLOCK) {
          s->observed_error = witnessed;
          return witnessed == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : witnessed;
        }
      }
    }
    h2_pal_webrtc_peer_close(a, s->peer);
    s->peer = NULL;
    memset(s->channels, 0, sizeof(s->channels));
    CALL(wait_released(s));
    if (!rejected) return H2_PAL_ERR_TIMEOUT;
    return H2_PAL_OK;
  }
  case H2_PAL_WEBRTC_E2E_EXPLICIT_STREAM_ID: {
    CALL(h2_pal_webrtc_peer_create_with_config(a, &custom, &s->peer));
    const h2_pal_webrtc_ice_server_t ice = {
        .url = {s->config->stun_url, strlen(s->config->stun_url)}};
    CALL(h2_pal_webrtc_peer_add_ice_server(a, s->peer, &ice));
    const h2_pal_webrtc_channel_config_t c = {.label = {"pal/id", 6u},
                                              .stream_id = 7u,
                                              .has_stream_id = 1,
                                              .negotiated = 1,
                                              .ordered = 1,
                                              .reliable = 1};
    CALL(h2_pal_webrtc_peer_create_data_channel(a, s->peer, &c,
                                                &s->channels[0]));
    CALL(h2_pal_webrtc_peer_start_offer(a, s->peer));
    CALL(wait_kind(s, H2_PAL_WEBRTC_EVENT_LOCAL_SDP, &s->held));
    size_t len = 0u;
    CALL(s->config->exchange_offer(s->config->fixture_user, s->held.sdp, &c,
                                   s->answer, sizeof(s->answer), &len));
    h2_pal_webrtc_event_release(&s->held);
    CALL(h2_pal_webrtc_peer_set_remote_sdp(
        a, s->peer, H2_PAL_WEBRTC_SDP_ANSWER,
        (h2_pal_webrtc_str_t){s->answer, len}));
    CALL(wait_kind_for(s, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE, &s->held,
                       connection_timeout(s)));
    REQUIRE(s->held.channel == s->channels[0] &&
            s->held.channel_state == H2_PAL_WEBRTC_CHANNEL_OPEN &&
            s->held.channel_info.has_stream_id &&
            s->held.channel_info.negotiated &&
            s->held.channel_info.stream_id == 7u);
    h2_pal_webrtc_event_release(&s->held);
    CALL(echo(s, 0, opus_tag, sizeof(opus_tag), 0, 0, 0));
    CALL(s->config->close_remote(s->config->fixture_user));
    h2_pal_webrtc_peer_close(a, s->peer);
    s->peer = NULL;
    memset(s->channels, 0, sizeof(s->channels));
    return wait_released(s);
  }
  default:
    return H2_PAL_ERR_INVALID_ARG;
  }
}
/* A separate real connection, after conformance, measures sustained liveness.
 * It deliberately uses no board speaker/microphone backend. */
static int run_soak(state_t *s, h2_pal_webrtc_e2e_soak_result_t *result) {
  result->requested_ms = s->config->soak_duration_ms;
  result->detail = H2_PAL_ERR_INVALID_STATE;
  s->opened = 0u;
  s->peer_state = H2_PAL_WEBRTC_PEER_NEW;
  s->track_bound = 0;
  h2_atomic_store(&s->track_state.detached, 0u);
  h2_atomic_store(&s->track_state.block, 0u);
  h2_atomic_store(&s->track_state.budget, 0u);
  const unsigned setup[] = {H2_PAL_WEBRTC_E2E_ALLOCATOR_OWNERSHIP,
      H2_PAL_WEBRTC_E2E_ICE_BORROWED_INPUT, H2_PAL_WEBRTC_E2E_TRACK_BIND,
      H2_PAL_WEBRTC_E2E_CHANNEL_CONFIGURATIONS,
      H2_PAL_WEBRTC_E2E_OFFER_OWNED_SDP, H2_PAL_WEBRTC_E2E_PION_NEGOTIATION,
      H2_PAL_WEBRTC_E2E_CHANNEL_METADATA};
  int rc = H2_PAL_OK;
  for (unsigned i = 0u; i < sizeof(setup) / sizeof(setup[0]); ++i) {
    rc = run_case(s, setup[i]);
    if (rc != H2_PAL_OK) goto finish;
  }
  const uint64_t start = now(s);
  uint64_t next_report = start;
  do {
    const uint64_t cycle = now(s);
    uint8_t payload[8] = {'H', '2', 'L', 'V'};
    const unsigned sequence = result->data_roundtrips;
    for (unsigned i = 0u; i < 4u; ++i)
      payload[4u + i] = (uint8_t)(sequence >> (i * 8u));
    rc = echo(s, 0u, payload, sizeof(payload), 0, 0, 0);
    if (rc != H2_PAL_OK) break;
    ++result->data_roundtrips;
    const unsigned writes = h2_atomic_load(&s->track_state.writes);
    rc = opus_send(s);
    if (rc == H2_PAL_OK) rc = wait_writes(s, writes + 1u);
    if (rc != H2_PAL_OK) break;
    ++result->opus_roundtrips;
    while (now(s) - cycle < 1000u) {
      h2_pal_webrtc_event_t event = {0};
      rc = poll_event(s, 20, &event);
      h2_pal_webrtc_event_release(&event);
      if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
        rc = H2_PAL_OK;
      if (rc != H2_PAL_OK) break;
    }
    result->elapsed_ms = now(s) - start;
    if (s->config->soak_report && now(s) >= next_report) {
      s->config->soak_report(s->config->report_user, result);
      next_report = now(s) + 10000u;
    }
  } while (rc == H2_PAL_OK && result->elapsed_ms < result->requested_ms);
  result->elapsed_ms = now(s) - start;
  result->completed = rc == H2_PAL_OK && result->elapsed_ms >= result->requested_ms &&
      result->data_roundtrips != 0u && result->data_roundtrips == result->opus_roundtrips;
finish:
  result->detail = rc;
  if (s->config->soak_report)
    s->config->soak_report(s->config->report_user, result);
  int close_rc = s->config->close_remote(s->config->fixture_user);
  return rc != H2_PAL_OK ? rc : close_rc;
}

#undef CALL
#undef REQUIRE

int h2_pal_webrtc_e2e_run(const h2_pal_webrtc_e2e_config_t *config,
                          h2_pal_webrtc_e2e_result_t *result) {
  if (result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(result, 0, sizeof(*result));
  for (unsigned i = 0; i < H2_PAL_WEBRTC_E2E_CASE_COUNT; ++i) {
    result->cases[i].id = case_ids[i];
    result->cases[i].blocked = 1;
    result->cases[i].detail = H2_PAL_ERR_UNSUPPORTED;
  }
  result->blocked = H2_PAL_WEBRTC_E2E_CASE_COUNT;
  if (!config || !config->runtime || !config->runtime->webrtc ||
      !complete(config->runtime->webrtc->vtable) || !config->runtime->mem ||
      !config->runtime->time || !config->runtime->time->vtable ||
      !config->runtime->time->vtable->get_monotonic_ms ||
      !config->runtime->time->vtable->sleep_ms || !config->stun_url ||
      !config->exchange_offer || !config->close_remote ||
      config->connection_timeout_ms > 120000u || config->soak_duration_ms > 3600000u)
    return H2_PAL_ERR_INVALID_ARG;
  state_t *s = h2_pal_mem_alloc(config->runtime->mem, sizeof(*s));
  if (!s)
    return H2_PAL_ERR_NO_MEMORY;
  memset(s, 0, sizeof(*s));
  s->config = config;
  s->api = config->runtime->webrtc;
  s->allocator.base = config->runtime->mem;
  h2_atomic_uint_t *counters[] = {
      &s->allocator.live,       &s->allocator.calls,
      &s->track_state.reads,    &s->track_state.writes,
      &s->track_state.attempts, &s->track_state.budget,
      &s->track_state.detached, &s->track_state.late,
      &s->track_state.block};
  unsigned initialized = 0u;
  int rc = H2_PAL_ERR_NO_MEMORY;
  for (; initialized < sizeof(counters) / sizeof(counters[0]); ++initialized)
    if (h2_atomic_uint_init(counters[initialized], 0u) != H2_ATOMIC_OK)
      goto cleanup_counters;
  if (h2_atomic_int_init(&s->allocator.fail, 0) != H2_ATOMIC_OK)
    goto cleanup_counters;
  s->memory = (h2_pal_mem_api_t){&s->allocator, &memory_vtable};
  s->track =
      (h2_pal_webrtc_track_t){.user = &s->track_state, .vtable = &track_vtable};
  rc = H2_PAL_OK;
  for (unsigned i = 0; i < H2_PAL_WEBRTC_E2E_CASE_COUNT; ++i) {
    h2_pal_webrtc_e2e_case_result_t *r = &result->cases[i];
    if (rc == H2_PAL_OK) {
      uint64_t start = now(s);
      s->line = 0;
      s->observed_error = s->authentication_evidence = 0;
      rc = run_case(s, i);
      r->elapsed_ms = now(s) - start;
      r->line = s->line;
      r->detail = rc;
      r->observed_error = s->observed_error;
      r->authentication_evidence = s->authentication_evidence;
      r->blocked = 0;
      r->passed = rc == H2_PAL_OK;
      --result->blocked;
      if (r->passed)
        ++result->passed;
      else
        ++result->failed;
    }
    if (config->report)
      config->report(config->report_user, r);
  }
  if (rc == H2_PAL_OK && config->soak_duration_ms != 0u)
    rc = run_soak(s, &result->soak);
  h2_atomic_store(&s->track_state.block, 0u);
  if (s->peer) {
    if (s->track_bound)
      (void)h2_pal_webrtc_peer_unset_track(s->api, s->peer, &s->track);
    h2_pal_webrtc_peer_close(s->api, s->peer);
  }
  h2_pal_webrtc_event_release(&s->held);
  (void)wait_released(s);
  result->retained_allocations = h2_atomic_load(&s->allocator.live);
  if (result->retained_allocations)
    return H2_PAL_ERR_INVALID_STATE;
  h2_atomic_int_destroy(&s->allocator.fail);
cleanup_counters:
  while (initialized)
    h2_atomic_uint_destroy(counters[--initialized]);
  /* Outstanding allocator ownership is a provider failure. Retain its
   * backing context rather than manufacture a use-after-free in the gate. */
  if (!result->retained_allocations)
    h2_pal_mem_free(config->runtime->mem, s);
  return rc == H2_PAL_OK && !result->blocked && !result->retained_allocations
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}
