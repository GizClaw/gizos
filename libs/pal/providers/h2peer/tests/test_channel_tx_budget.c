#include "h2_peer_internal.h"

// These tests use assertions for both checks and the operations under test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Drives the network round's channel scheduler directly, without a transport:
 * with no production peer connection every send resolves as INVALID_STATE,
 * which the scheduler consumes like a delivered message. That isolates the
 * ring, the round budget and the round-robin order from SCTP behaviour. */

enum { CHANNEL_MAX = 3 };

typedef struct fixture {
  h2_peer_t owner;
  h2_pal_webrtc_peer_t peer;
  h2_pal_webrtc_channel_t channels[CHANNEL_MAX];
  h2_pal_mem_api_t mem;
  h2_pal_time_api_t time;
  size_t allocations;
  size_t frees;
  uint64_t now_us;
} fixture_t;

static h2_pal_result_t monotonic_us(void *user, uint64_t *out_us) {
  *out_us = ((fixture_t *)user)->now_us;
  return H2_PAL_OK;
}

static void *allocate(void *user, size_t size) {
  fixture_t *f = user;
  void *p = malloc(size);
  if (p != NULL)
    ++f->allocations;
  return p;
}

static void deallocate(void *user, void *p) {
  fixture_t *f = user;
  if (p != NULL)
    ++f->frees;
  free(p);
}

static void initialize(fixture_t *f, size_t channel_count) {
  static const h2_pal_mem_vtable_t memory = {.alloc = allocate,
                                             .free = deallocate};
  static const h2_pal_time_vtable_t time = {.get_monotonic_us = monotonic_us};
  memset(f, 0, sizeof(*f));
  f->mem = (h2_pal_mem_api_t){.user = f, .vtable = &memory};
  f->time = (h2_pal_time_api_t){.user = f, .vtable = &time};
  f->owner.config.mem = &f->mem;
  f->owner.config.time = &f->time;
  f->peer.owner = &f->owner;
  atomic_init(&f->owner.refs, 1u);
  atomic_init(&f->peer.refs, 1u);
  atomic_init(&f->peer.state, H2_PAL_WEBRTC_PEER_CONNECTED);
  atomic_init(&f->peer.closed, 0);
  atomic_init(&f->peer.network_transport_result, H2_PAL_OK);
  atomic_init(&f->peer.network_error_reported, 0);
  atomic_init(&f->peer.network_send_wakeup_queued, 0);
  atomic_init(&f->peer.channel_ready, 0u);
  atomic_init(&f->peer.network_event_count, 0u);
  atomic_init(&f->peer.network_event_bytes, 0u);
  assert(channel_count <= CHANNEL_MAX);
  for (size_t i = 0u; i < channel_count; ++i) {
    h2_pal_webrtc_channel_t *channel = &f->channels[i];
    channel->owner = &f->peer;
    atomic_init(&channel->open, 1);
    atomic_init(&channel->terminal, 0);
    atomic_init(&channel->ready_slot, (uint8_t)i);
    for (size_t slot = 0u; slot < H2_PEER_INPUT_SLOT_COUNT; ++slot)
      atomic_init(&channel->tx_state[slot], 0u);
    atomic_init(&channel->tx_head, 0u);
    atomic_init(&channel->tx_tail, 0u);
    atomic_init(&channel->tx_ready_since_us, 0u);
    channel->next = i + 1u < channel_count ? &f->channels[i + 1u] : NULL;
  }
  f->peer.channels = channel_count != 0u ? &f->channels[0] : NULL;
}

static void cleanup(fixture_t *f) {
  for (size_t i = 0u; i < CHANNEL_MAX; ++i) {
    for (size_t slot = 0u; slot < H2_PEER_INPUT_SLOT_COUNT; ++slot) {
      h2_peer_tx_item_t *item = f->channels[i].tx_storage[slot];
      if (item != NULL) {
        deallocate(f, item->data);
        deallocate(f, item);
      }
    }
  }
  assert(f->allocations == f->frees);
}

static h2_pal_result_t push(fixture_t *f, size_t channel, size_t len) {
  static uint8_t payload[16384];
  assert(len <= sizeof(payload));
  memset(payload, (int)('a' + channel), len);
  return h2_peer_channel_tx_push(&f->channels[channel], payload, len, 0);
}

static size_t queued(const fixture_t *f, size_t channel) {
  size_t count = 0u;
  for (size_t slot = 0u; slot < H2_PEER_INPUT_SLOT_COUNT; ++slot)
    if (atomic_load(&f->channels[channel].tx_state[slot]) == 2u)
      ++count;
  return count;
}

static uint32_t take_snapshot(fixture_t *f) {
  return atomic_exchange(&f->peer.channel_ready, 0u);
}

/* The ring accepts exactly H2_PEER_INPUT_SLOT_COUNT messages, drains them in
 * FIFO order, and recycles slots (with their buffers) across the wrap. */
static void ring_depth_fifo_and_reuse(void) {
  fixture_t f;
  initialize(&f, 1u);
  for (size_t i = 0u; i < H2_PEER_INPUT_SLOT_COUNT; ++i)
    assert(push(&f, 0u, 100u + i) == H2_PAL_OK);
  assert(push(&f, 0u, 1u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(queued(&f, 0u) == H2_PEER_INPUT_SLOT_COUNT);
  assert(take_snapshot(&f) == 1u);
  const size_t live_after_fill = f.allocations - f.frees;

  uint32_t snapshot = 1u;
  assert(h2_peer_network_service_channel(&f.peer, &snapshot) == 1);
  assert(snapshot == 0u);
  assert(queued(&f, 0u) == 0u);
  assert(f.peer.perf_channel_service_count == H2_PEER_INPUT_SLOT_COUNT);
  assert(f.peer.perf_channel_send_blocked == 0u);
  assert(atomic_load(&f.channels[0].tx_head) == 0u);
  assert(atomic_load(&f.channels[0].tx_tail) == 0u);

  // Refill past the wrap point: no new allocations for same-sized payloads.
  for (size_t i = 0u; i < H2_PEER_INPUT_SLOT_COUNT; ++i)
    assert(push(&f, 0u, 100u + i) == H2_PAL_OK);
  assert(f.allocations - f.frees == live_after_fill);
  snapshot = take_snapshot(&f);
  assert(snapshot == 1u);
  assert(h2_peer_network_service_channel(&f.peer, &snapshot) == 1);
  assert(queued(&f, 0u) == 0u);
  assert(f.peer.perf_channel_service_count == 2u * H2_PEER_INPUT_SLOT_COUNT);

  // Nothing queued: no progress, and the stale bit is dropped.
  snapshot = 1u;
  assert(h2_peer_network_service_channel(&f.peer, &snapshot) == 0);
  assert(snapshot == 0u);
  cleanup(&f);
}

/* A round stops once the byte budget is reached, keeps the channel's ready
 * bit so the next round resumes, and drains in FIFO order. */
static void byte_budget_bounds_a_round(void) {
  fixture_t f;
  initialize(&f, 1u);
  assert(push(&f, 0u, 100u) == H2_PAL_OK);
  assert(push(&f, 0u, H2_PEER_NETWORK_CHANNEL_ROUND_BYTES) == H2_PAL_OK);
  assert(push(&f, 0u, 100u) == H2_PAL_OK);
  assert(push(&f, 0u, 100u) == H2_PAL_OK);
  uint32_t snapshot = take_snapshot(&f);

  assert(h2_peer_network_service_channel(&f.peer, &snapshot) == 1);
  assert(f.peer.perf_channel_service_count == 2u);
  assert(snapshot == 1u);
  assert(queued(&f, 0u) == 2u);
  assert(atomic_load(&f.channels[0].tx_head) == 2u);

  assert(h2_peer_network_service_channel(&f.peer, &snapshot) == 1);
  assert(f.peer.perf_channel_service_count == 4u);
  assert(snapshot == 0u);
  assert(queued(&f, 0u) == 0u);
  cleanup(&f);
}

/* Channels alternate one message at a time, so a bulk sender shares each
 * round with a channel carrying small messages. */
static void channels_alternate_within_the_budget(void) {
  fixture_t f;
  initialize(&f, 2u);
  const size_t bulk = H2_PEER_NETWORK_CHANNEL_ROUND_BYTES / 2u - 200u;
  for (size_t i = 0u; i < H2_PEER_INPUT_SLOT_COUNT; ++i) {
    assert(push(&f, 0u, bulk) == H2_PAL_OK);
    assert(push(&f, 1u, 100u) == H2_PAL_OK);
  }
  uint32_t snapshot = take_snapshot(&f);
  assert(snapshot == 3u);

  // Round 1: A0 B0 A1 B1 A2 -> bytes >= budget after A2.
  assert(h2_peer_network_service_channel(&f.peer, &snapshot) == 1);
  assert(f.peer.perf_channel_service_count == 5u);
  assert(queued(&f, 0u) == 1u);
  assert(queued(&f, 1u) == 2u);
  assert(snapshot == 3u);
  assert(f.peer.channel_round_robin == 1u);

  // Round 2 starts with B (round robin), then A3 empties A: B2 A3 B3.
  assert(h2_peer_network_service_channel(&f.peer, &snapshot) == 1);
  assert(f.peer.perf_channel_service_count == 8u);
  assert(queued(&f, 0u) == 0u);
  assert(queued(&f, 1u) == 0u);
  assert(snapshot == 0u);
  cleanup(&f);
}

/* Many tiny messages are bounded by the message budget, not only bytes. */
static void message_budget_bounds_a_round(void) {
  fixture_t f;
  initialize(&f, CHANNEL_MAX);
  const size_t total = CHANNEL_MAX * H2_PEER_INPUT_SLOT_COUNT;
  assert(total > H2_PEER_NETWORK_CHANNEL_ROUND_MESSAGES);
  for (size_t i = 0u; i < H2_PEER_INPUT_SLOT_COUNT; ++i)
    for (size_t channel = 0u; channel < CHANNEL_MAX; ++channel)
      assert(push(&f, channel, 1u) == H2_PAL_OK);
  uint32_t snapshot = take_snapshot(&f);
  assert(snapshot == 7u);

  assert(h2_peer_network_service_channel(&f.peer, &snapshot) == 1);
  assert(f.peer.perf_channel_service_count ==
         H2_PEER_NETWORK_CHANNEL_ROUND_MESSAGES);
  size_t remaining = 0u;
  for (size_t channel = 0u; channel < CHANNEL_MAX; ++channel)
    remaining += queued(&f, channel);
  assert(remaining == total - H2_PEER_NETWORK_CHANNEL_ROUND_MESSAGES);

  size_t rounds = 1u;
  while (snapshot != 0u) {
    assert(h2_peer_network_service_channel(&f.peer, &snapshot) == 1);
    ++rounds;
    assert(rounds <= 4u);
  }
  assert(f.peer.perf_channel_service_count == total);
  cleanup(&f);
}

/* A message queued behind others reports the head-of-line wait. */
static void ready_latency_tracks_the_oldest_message(void) {
  fixture_t f;
  initialize(&f, 1u);
  f.now_us = 1000u;
  assert(push(&f, 0u, 10u) == H2_PAL_OK);
  f.now_us = 5000u;
  assert(push(&f, 0u, 10u) == H2_PAL_OK);
  assert(atomic_load(&f.channels[0].tx_ready_since_us) == 1000u);
  uint32_t snapshot = take_snapshot(&f);
  f.now_us = 6000u;
  assert(h2_peer_network_service_channel(&f.peer, &snapshot) == 1);
  assert(f.peer.perf_channel_max_ready_us == 5000u);
  assert(atomic_load(&f.channels[0].tx_ready_since_us) == 0u);
  cleanup(&f);
}

int main(void) {
  ring_depth_fifo_and_reuse();
  byte_budget_bounds_a_round();
  channels_alternate_within_the_budget();
  message_budget_bounds_a_round();
  ready_latency_tracks_the_oldest_message();
  return 0;
}
