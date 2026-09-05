#include "h2_peer_internal.h"

// These tests use assertions for both checks and the operations under test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  h2_peer_t owner;
  h2_pal_webrtc_peer_t peer;
  h2_pal_mem_api_t mem;
  h2_pal_webrtc_track_t track;
  h2_pal_result_t write_result;
  size_t calls;
  size_t accepted;
  uint8_t values[128];
  size_t allocations;
  size_t frees;
  int fail_next;
  uint64_t now_us;
  h2_pal_time_api_t time;
  h2_pal_log_api_t log;
} fixture_t;

static h2_pal_result_t monotonic_us(void *user, uint64_t *out_us) {
  *out_us = ((fixture_t *)user)->now_us;
  return H2_PAL_OK;
}

static int capture_log(void *user, h2_pal_log_level_t level, const char *scope,
                       const char *message) {
  (void)user;
  (void)level;
  (void)scope;
  (void)message;
  return H2_PAL_OK;
}

static void *allocate(void *user, size_t size) {
  fixture_t *f = user;
  if (f->fail_next) {
    f->fail_next = 0;
    return NULL;
  }
  void *p = malloc(size);
  if (p != NULL)
    ++f->allocations;
  return p;
}

static void deallocate(void *user, void *p) {
  if (p != NULL) {
    ++((fixture_t *)user)->frees;
    free(p);
  }
}

static h2_pal_result_t write_frame(void *user, const uint8_t *opus,
                                   size_t len) {
  fixture_t *f = user;
  ++f->calls;
  if (f->write_result != H2_PAL_OK)
    return f->write_result;
  assert(len == 0u || len == 1u);
  assert(f->accepted < sizeof(f->values));
  if (len == 0u)
    assert(opus == NULL);
  f->values[f->accepted++] = len == 0u ? 0xffu : opus[0];
  return H2_PAL_OK;
}

static void initialize(fixture_t *f) {
  static const h2_pal_mem_vtable_t memory = {.alloc = allocate,
                                             .free = deallocate};
  static const h2_pal_webrtc_track_vtable_t track = {.write = write_frame};
  static const h2_pal_time_vtable_t time = {.get_monotonic_us = monotonic_us};
  static const h2_pal_log_vtable_t log = {.write = capture_log};
  memset(f, 0, sizeof(*f));
  f->mem = (h2_pal_mem_api_t){.user = f, .vtable = &memory};
  f->owner.config.mem = &f->mem;
  f->time = (h2_pal_time_api_t){.user = f, .vtable = &time};
  f->log = (h2_pal_log_api_t){.user = f, .vtable = &log};
  f->owner.config.time = &f->time;
  f->owner.config.log = &f->log;
  f->track = (h2_pal_webrtc_track_t){.user = f, .vtable = &track};
  f->peer.owner = &f->owner;
  f->peer.media_track = &f->track;
  atomic_init(&f->owner.refs, 1u);
  atomic_init(&f->peer.refs, 1u);
  atomic_init(&f->peer.state, H2_PAL_WEBRTC_PEER_CONNECTED);
  atomic_init(&f->peer.closed, 0);
  atomic_init(&f->peer.network_transport_result, H2_PAL_OK);
  atomic_init(&f->peer.network_error_reported, 0);
  atomic_init(&f->peer.network_event_count, 0u);
  atomic_init(&f->peer.network_event_bytes, 0u);
  f->write_result = H2_PAL_ERR_WOULD_BLOCK;
}

static void cleanup(fixture_t *f);

static void cleanup(fixture_t *f) {
  h2_peer_webrtc_discard_media(&f->peer);
  assert(f->peer.media_receive_head == NULL &&
         f->peer.media_receive_tail == NULL);
  assert(f->peer.media_receive_count == 0u);
  assert(f->allocations == f->frees);
  assert(atomic_load(&f->owner.refs) == 1u);
  assert(atomic_load(&f->peer.refs) == 1u);
}

static void fifo_and_packet_loss(void) {
  fixture_t f;
  initialize(&f);
  uint8_t packet = 0x12;
  h2_peer_webrtc_emit_opus_frame(&f.peer, &packet, 1u);
  packet = 0x34;
  h2_peer_webrtc_emit_opus_frame(&f.peer, NULL, 0u);
  h2_peer_webrtc_emit_opus_frame(&f.peer, &packet, 1u);
  packet = 0x56;
  assert(f.calls == 1u && f.peer.media_receive_count == 3u);
  assert(atomic_load(&f.peer.network_transport_result) == H2_PAL_OK);
  assert(h2_peer_webrtc_service_media(&f.peer) == H2_PAL_OK);
  assert(f.calls == 2u && f.peer.media_receive_count == 3u);
  f.write_result = H2_PAL_OK;
  assert(h2_peer_webrtc_service_media(&f.peer) == H2_PAL_OK);
  assert(f.accepted == 3u && f.values[0] == 0x12 && f.values[1] == 0xff &&
         f.values[2] == 0x34);
  assert(f.peer.media_receive_count == 0u);
  cleanup(&f);
}

static void limits_and_cleanup(void) {
  fixture_t f;
  initialize(&f);
  uint8_t packet = 0;
  for (size_t i = 0; i < H2_PEER_MEDIA_RECEIVE_LIMIT; ++i) {
    packet = (uint8_t)i;
    h2_peer_webrtc_emit_opus_frame(&f.peer, &packet, 1u);
  }
  assert(f.peer.media_receive_count == H2_PEER_MEDIA_RECEIVE_LIMIT &&
         f.calls == 1u);
  f.write_result = H2_PAL_OK;
  assert(h2_peer_webrtc_service_media(&f.peer) == H2_PAL_OK);
  assert(f.accepted == 8u &&
         f.peer.media_receive_count == H2_PEER_MEDIA_RECEIVE_LIMIT - 8u);
  while (f.peer.media_receive_count != 0u)
    assert(h2_peer_webrtc_service_media(&f.peer) == H2_PAL_OK);
  for (size_t i = 0; i < H2_PEER_MEDIA_RECEIVE_LIMIT; ++i)
    assert(f.values[i] == i);
  cleanup(&f);

  initialize(&f);
  for (size_t i = 0; i <= H2_PEER_MEDIA_RECEIVE_LIMIT; ++i) {
    packet = (uint8_t)i;
    h2_peer_webrtc_emit_opus_frame(&f.peer, &packet, 1u);
  }
  assert(f.peer.media_receive_count == H2_PEER_MEDIA_RECEIVE_LIMIT);
  assert(atomic_load(&f.peer.network_transport_result) == H2_PAL_OK);
  f.write_result = H2_PAL_OK;
  while (f.peer.media_receive_count != 0u)
    assert(h2_peer_webrtc_service_media(&f.peer) == H2_PAL_OK);
  assert(f.accepted == H2_PEER_MEDIA_RECEIVE_LIMIT && f.values[0] == 1u &&
         f.values[H2_PEER_MEDIA_RECEIVE_LIMIT - 1u] ==
             H2_PEER_MEDIA_RECEIVE_LIMIT);
  cleanup(&f);
}

static void failures(void) {
  fixture_t f;
  initialize(&f);
  const uint8_t packet = 0x42;
  f.fail_next = 1;
  h2_peer_webrtc_emit_opus_frame(&f.peer, &packet, 1u);
  assert(atomic_load(&f.peer.network_transport_result) == H2_PAL_ERR_NO_MEMORY);
  assert(f.peer.media_receive_count == 0u);
  cleanup(&f);

  initialize(&f);
  h2_peer_webrtc_emit_opus_frame(&f.peer, &packet, 1u);
  f.write_result = H2_PAL_ERR_IO;
  assert(h2_peer_webrtc_service_media(&f.peer) == H2_PAL_ERR_IO);
  assert(f.peer.media_receive_count == 0u && f.calls == 2u);
  assert(h2_peer_webrtc_service_media(&f.peer) == H2_PAL_ERR_IO &&
         f.calls == 2u);
  cleanup(&f);

  initialize(&f);
  h2_peer_webrtc_emit_opus_frame(&f.peer, &packet,
                                 H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE + 1u);
  assert(atomic_load(&f.peer.network_transport_result) == H2_PAL_ERR_FORMAT &&
         f.calls == 0u);
  cleanup(&f);

  initialize(&f);
  h2_peer_webrtc_emit_opus_frame(&f.peer, &packet, 1u);
  h2_peer_webrtc_discard_media(&f.peer);
  f.track.vtable = NULL;
  assert(h2_peer_webrtc_service_media(&f.peer) == H2_PAL_OK && f.calls == 1u);
  cleanup(&f);
}

int main(void) {
  fifo_and_packet_loss();
  limits_and_cleanup();
  failures();
  return 0;
}
