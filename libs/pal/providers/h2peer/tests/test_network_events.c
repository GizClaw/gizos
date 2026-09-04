#include "h2/pal/h2_pal_unsupported.h"
#include "h2_desktop_platform.h"
#include "h2_peer_internal.h"

// These tests use assertions for both checks and the operations under test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

// Use the production constructor and real PAL tasks/queues. Inject only at the
// protocol callback boundary; no offer or network connection is created here.
typedef struct fixture {
  h2_peer_t *owner;
  h2_pal_webrtc_peer_t *peer;
  const h2_pal_webrtc_api_t *api;
  h2_pal_mem_api_t mem;
  h2_pal_queue_api_t queue;
  h2_pal_queue_vtable_t queue_vtable;
  h2_pal_queue_t *events;
  size_t capacity;
  atomic_int fail_allocations;
  atomic_size_t allocations;
  atomic_size_t frees;
  atomic_int waiting;
  atomic_int done;
  h2_pal_result_t poll_result;
  h2_pal_webrtc_event_t event;
  h2_pal_result_t write_result;
  unsigned writes;
} fixture_t;

static const h2_pal_queue_api_t *real_queue;
static fixture_t *active;

static void *allocate(void *user, size_t size) {
  fixture_t *f = user;
  int left = atomic_load(&f->fail_allocations);
  while (left > 0) {
    if (atomic_compare_exchange_weak(&f->fail_allocations, &left, left - 1))
      return NULL;
  }
  void *p = malloc(size);
  if (p != NULL)
    atomic_fetch_add(&f->allocations, 1u);
  return p;
}

static void deallocate(void *user, void *p) {
  if (p != NULL) {
    atomic_fetch_add(&((fixture_t *)user)->frees, 1u);
    free(p);
  }
}

static int create_queue(void *user, const h2_pal_queue_config_t *config,
                        h2_pal_queue_t **out) {
  int result = real_queue->vtable->create(user, config, out);
  if (result == H2_PAL_OK && strcmp(config->name, "h2peer/net/events") == 0) {
    active->events = *out;
    active->capacity = config->item_count;
  }
  return result;
}

static int receive(void *user, h2_pal_queue_t *queue, void *out,
                   uint32_t timeout_ms) {
  if (queue == active->events && timeout_ms != H2_PAL_QUEUE_NO_WAIT)
    atomic_store(&active->waiting, 1);
  return real_queue->vtable->recv(user, queue, out, timeout_ms);
}

static h2_pal_result_t write_frame(void *user, const uint8_t *data,
                                   size_t len) {
  fixture_t *f = user;
  assert(data != NULL && len == 1u);
  ++f->writes;
  return f->write_result;
}

static void initialize(fixture_t *f) {
  static const h2_pal_mem_vtable_t memory = {.alloc = allocate,
                                             .free = deallocate};
  memset(f, 0, sizeof(*f));
  atomic_init(&f->fail_allocations, 0);
  atomic_init(&f->allocations, 0u);
  atomic_init(&f->frees, 0u);
  atomic_init(&f->waiting, 0);
  atomic_init(&f->done, 0);
  f->mem = (h2_pal_mem_api_t){.user = f, .vtable = &memory};
  real_queue = h2_desktop_platform_queue_api();
  f->queue_vtable = *real_queue->vtable;
  f->queue_vtable.create = create_queue;
  f->queue_vtable.recv = receive;
  f->queue = (h2_pal_queue_api_t){real_queue->user, &f->queue_vtable};
  active = f;
  const h2_peer_config_t config = {
      .mem = &f->mem,
      .control_mem = h2_desktop_platform_default_allocator(),
      .log = h2_desktop_platform_log_api(),
      .net = h2_pal_unsupported_net_api(),
      .queue = &f->queue,
      .sync = h2_desktop_platform_sync_api(),
      .task = h2_desktop_platform_task_api(),
      .time = h2_desktop_platform_time_api(),
      .crypto = h2_pal_unsupported_crypto_api(),
      .dtls = h2_pal_unsupported_dtls_api(),
      .sctp = h2_pal_unsupported_sctp_api(),
  };
  assert(h2_peer_create(&config, &f->owner) == H2_PAL_OK);
  f->api = h2_peer_webrtc_api(f->owner);
  assert(h2_pal_webrtc_peer_create(f->api, &f->peer) == H2_PAL_OK);
  assert(f->capacity > 0u && f->capacity < 1024u);
}

static void cleanup(fixture_t *f) {
  h2_pal_webrtc_peer_close(f->api, f->peer);
  h2_peer_destroy(&f->owner);
  assert(f->owner == NULL);
  assert(atomic_load(&f->allocations) == atomic_load(&f->frees));
  active = NULL;
}

static void emit_sdp(fixture_t *f, uint16_t sequence) {
  char payload[2] = {(char)(sequence >> 8u), (char)sequence};
  h2_peer_webrtc_emit_local_sdp(
      f->peer, H2_PAL_WEBRTC_SDP_OFFER,
      (h2_pal_webrtc_str_t){payload, sizeof(payload)});
}

static void expect_sdp(fixture_t *f, uint16_t sequence) {
  h2_pal_webrtc_event_t event = {0};
  assert(h2_pal_webrtc_peer_poll(f->api, f->peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_LOCAL_SDP && event.sdp.len == 2u);
  assert((uint8_t)event.sdp.data[0] == (uint8_t)(sequence >> 8u));
  assert((uint8_t)event.sdp.data[1] == (uint8_t)sequence);
  h2_pal_webrtc_event_release(&event);
}

static void expect_error(fixture_t *f, h2_pal_result_t error, int owned) {
  h2_pal_webrtc_event_t event = {0};
  assert(h2_pal_webrtc_peer_poll(f->api, f->peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_ERROR && event.error == error);
  assert((event._private != NULL) == owned);
  h2_pal_webrtc_event_release(&event);
  assert(h2_pal_webrtc_peer_poll(f->api, f->peer, 0, &event) == error);
  assert(event.kind == 0 && event._private == NULL);
}

static void error_fallbacks(void) {
  fixture_t f;
  initialize(&f);
  emit_sdp(&f, 42u);
  // First allocation fails for SDP, the second for its ERROR notification.
  atomic_store(&f.fail_allocations, 2);
  emit_sdp(&f, 43u);
  expect_sdp(&f, 42u);
  expect_error(&f, H2_PAL_ERR_NO_MEMORY, 0);
  cleanup(&f);

  initialize(&f);
  h2_pal_webrtc_channel_t *channel = NULL;
  const h2_pal_webrtc_channel_config_t channel_config = {
      .label = {"test", 4u},
      .ordered = 1,
      .reliable = 1,
  };
  assert(h2_pal_webrtc_peer_create_data_channel(f.api, f.peer, &channel_config,
                                                &channel) == H2_PAL_OK);
  for (size_t i = 0u; i < f.capacity; ++i)
    emit_sdp(&f, (uint16_t)i);
  assert(atomic_load(&f.peer->network_transport_result) == H2_PAL_OK);
  const uint8_t message = 0x42;
  assert(h2_peer_webrtc_emit_channel_message(f.peer, channel, &message, 1u,
                                             0) == H2_PAL_ERR_WOULD_BLOCK);
  assert(atomic_load(&f.peer->network_transport_result) == H2_PAL_OK);
  expect_sdp(&f, 0u);
  assert(h2_peer_webrtc_emit_channel_message(f.peer, channel, &message, 1u,
                                             0) == H2_PAL_OK);
  for (size_t i = 1u; i < f.capacity; ++i)
    expect_sdp(&f, (uint16_t)i);
  h2_pal_webrtc_event_t event = {0};
  assert(h2_pal_webrtc_peer_poll(f.api, f.peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE);
  assert(event.channel == channel && event.data_len == 1u &&
         event.data[0] == message);
  h2_pal_webrtc_event_release(&event);
  assert(h2_pal_webrtc_peer_poll(f.api, f.peer, 0, &event) ==
         H2_PAL_ERR_WOULD_BLOCK);
  for (size_t i = 0u; i < f.capacity; ++i)
    emit_sdp(&f, (uint16_t)i);
  emit_sdp(&f, (uint16_t)f.capacity);
  h2_pal_result_t failure = atomic_load(&f.peer->network_transport_result);
  assert(failure == H2_PAL_ERR_NO_SPACE);
  for (size_t i = 0u; i < f.capacity; ++i)
    expect_sdp(&f, (uint16_t)i);
  expect_error(&f, failure, 0);
  cleanup(&f);
}

static void poll_waiter(void *user) {
  fixture_t *f = user;
  f->poll_result = h2_pal_webrtc_peer_poll(f->api, f->peer, 5000, &f->event);
  atomic_store(&f->done, 1);
}

static void await_flag(atomic_int *flag) {
  for (unsigned i = 0u; i < 1000u && !atomic_load(flag); ++i)
    assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u) ==
           H2_PAL_OK);
  // A failure aborts this isolated process instead of leaving a task with a
  // dangling stack fixture. Do not wait the full 5-second poll timeout here.
  assert(atomic_load(flag));
}

static void wake_and_unset(void) {
  fixture_t f;
  initialize(&f);
  h2_pal_webrtc_channel_t *channel = NULL;
  const h2_pal_webrtc_channel_config_t channel_config = {
      .label = {"test", 4u},
      .ordered = 1,
      .reliable = 1,
  };
  assert(h2_pal_webrtc_peer_create_data_channel(f.api, f.peer, &channel_config,
                                                &channel) == H2_PAL_OK);
  static const h2_pal_webrtc_track_vtable_t vtable = {.write = write_frame};
  h2_pal_webrtc_track_t a = {.user = &f, .vtable = &vtable}, b = a;
  const uint8_t packet = 0x42;
  assert(h2_pal_webrtc_peer_set_track(f.api, f.peer, &a) == H2_PAL_OK);
  f.write_result = H2_PAL_ERR_WOULD_BLOCK;
  h2_peer_webrtc_emit_opus_frame(f.peer, &packet, 1u);
  assert(f.peer->media_receive_count == 1u && f.writes == 1u);
  assert(h2_pal_webrtc_peer_unset_track(f.api, f.peer, &b) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(f.peer->media_receive_count == 1u);
  assert(h2_pal_webrtc_peer_unset_track(f.api, f.peer, &a) == H2_PAL_OK);
  a.vtable = NULL;
  assert(f.peer->media_receive_count == 0u);
  assert(h2_pal_webrtc_peer_set_track(f.api, f.peer, &b) == H2_PAL_OK);
  f.write_result = H2_PAL_ERR_IO;
  h2_pal_task_t *waiter = NULL;
  const h2_pal_task_options_t options = {.name = "h2peer/test/poll"};
  assert(h2_pal_task_start(h2_desktop_platform_task_api(), &options,
                           poll_waiter, &f, &waiter) == H2_PAL_OK);
  await_flag(&f.waiting);
  h2_peer_webrtc_emit_opus_frame(f.peer, &packet, 1u);
  await_flag(&f.done);
  assert(h2_pal_task_join(h2_desktop_platform_task_api(), waiter) == H2_PAL_OK);
  assert(f.poll_result == H2_PAL_OK &&
         f.event.kind == H2_PAL_WEBRTC_EVENT_ERROR);
  assert(f.event.error == H2_PAL_ERR_IO && f.event._private != NULL);
  assert(f.writes == 2u);
  assert(h2_pal_webrtc_peer_send_opus(f.api, f.peer, &packet, 1u) ==
         H2_PAL_ERR_IO);
  assert(h2_pal_webrtc_channel_send(f.api, channel, &packet, 1u, 0) ==
         H2_PAL_ERR_IO);
  assert(atomic_load(&f.peer->rtp_pending) == NULL);
  assert(atomic_load(&channel->tx_state[0]) == 0u);
  // An existing fatal error must not turn a successful detach into failure.
  assert(h2_pal_webrtc_peer_unset_track(f.api, f.peer, &b) == H2_PAL_OK);
  b.vtable = NULL;
  h2_pal_webrtc_event_t next = {0};
  assert(h2_pal_webrtc_peer_poll(f.api, f.peer, 0, &next) == H2_PAL_ERR_IO);
  h2_pal_webrtc_peer_close(f.api, f.peer);
  h2_peer_destroy(&f.owner);
  assert(f.owner == NULL);
  // The owned error remains releasable after peer/provider destruction.
  assert(f.event.error == H2_PAL_ERR_IO);
  h2_pal_webrtc_event_release(&f.event);
  assert(atomic_load(&f.allocations) == atomic_load(&f.frees));
  active = NULL;
}

int main(void) {
  error_fallbacks();
  wake_and_unset();
  return 0;
}
