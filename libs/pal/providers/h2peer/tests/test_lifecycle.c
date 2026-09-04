#include "h2/pal/h2_pal_unsupported.h"
#include "h2_desktop_platform.h"
#include "h2_peer_internal.h"
#include "providers/h2_peer_portable_backend.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/*
 * Link the exact production webrtc_core with a protocol-boundary test double.
 * Every public operation still uses the real PAL network task and queues.
 * The double never implements a second peer API or drives work from app poll.
 */
typedef struct fixture {
  h2_peer_config_t config;
  h2_pal_mem_api_t mem;
  h2_pal_task_api_t task;
  h2_peer_t *owner;
  h2_pal_webrtc_peer_t *peer;
  const h2_pal_webrtc_api_t *api;
  h2_pal_queue_t *resets;
  atomic_size_t attempts, allocations, frees, fail_at;
  atomic_uint starts, joins, polls, connected, opens, sends, opus_sends;
  atomic_uint reset_attempts, submitted, processed, forgotten;
  atomic_int offer_result, answer_result, poll_result, reset_result,
      open_result;
  atomic_int terminal_next_open, send_busy, opus_busy, poll_gate, poll_entered;
  atomic_int track_ready;
  atomic_uint track_reads, track_writes;
  atomic_int remote_sid, remote_result;
  atomic_uint remote_processed;
  atomic_int fail_joins;
  atomic_int sent_since_poll, async_receive;
  atomic_uint post_send_polls, post_send_waits, idle_waits;
} fixture_t;

static _Thread_local int in_protocol_task;

typedef struct task_entry {
  h2_pal_task_entry_t entry;
  void *context;
} task_entry_t;

static void enter_task(void *user) {
  task_entry_t entry = *(task_entry_t *)user;
  free(user);
  in_protocol_task = 1;
  entry.entry(entry.context);
  in_protocol_task = 0;
}

static int start_task(void *user, const h2_pal_task_options_t *options,
                      h2_pal_task_entry_t entry, void *context,
                      h2_pal_task_t **out) {
  fixture_t *f = user;
  task_entry_t *call = malloc(sizeof(*call));
  assert(call != NULL);
  *call = (task_entry_t){entry, context};
  int rc = h2_pal_task_start(h2_desktop_platform_task_api(), options,
                             enter_task, call, out);
  if (rc == H2_PAL_OK)
    atomic_fetch_add(&f->starts, 1u);
  else
    free(call);
  return rc;
}

static int join_task(void *user, h2_pal_task_t *task) {
  fixture_t *f = user;
  if (atomic_exchange(&f->fail_joins, 0))
    return H2_PAL_ERR_IO;
  int rc = h2_pal_task_join(h2_desktop_platform_task_api(), task);
  if (rc == H2_PAL_OK)
    atomic_fetch_add(&((fixture_t *)user)->joins, 1u);
  return rc;
}

static void *allocate(void *user, size_t size) {
  fixture_t *f = user;
  size_t attempt = atomic_fetch_add(&f->attempts, 1u) + 1u;
  if (attempt == atomic_load(&f->fail_at))
    return NULL;
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

static fixture_t *protocol(h2_pal_webrtc_peer_t *peer) {
  assert(in_protocol_task);
  return peer->owner->config.mem->user;
}

h2_pal_result_t h2_peer_portable_start_offer(h2_pal_webrtc_peer_t *peer) {
  fixture_t *f = protocol(peer);
  h2_pal_result_t rc = atomic_load(&f->offer_result);
  if (rc != H2_PAL_OK)
    return rc;
  peer->production_pc = f;
  peer->offer_started = 1;
  h2_peer_webrtc_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_CONNECTING);
  h2_peer_webrtc_emit_local_sdp(peer, H2_PAL_WEBRTC_SDP_OFFER,
                                (h2_pal_webrtc_str_t){"offer", 5u});
  return H2_PAL_OK;
}

h2_pal_result_t h2_peer_portable_set_remote_sdp(h2_pal_webrtc_peer_t *peer,
                                                h2_pal_webrtc_sdp_type_t type,
                                                h2_pal_webrtc_str_t sdp) {
  fixture_t *f = protocol(peer);
  if (type != H2_PAL_WEBRTC_SDP_ANSWER || sdp.len != 6u ||
      memcmp(sdp.data, "answer", 6u) != 0)
    return H2_PAL_ERR_FORMAT;
  h2_pal_result_t rc = atomic_load(&f->answer_result);
  if (rc == H2_PAL_OK)
    peer->remote_answer_set = 1;
  return rc;
}

static void record_transport_wait(fixture_t *f, int timeout_ms) {
  assert(timeout_ms >= 0);
  if (atomic_exchange(&f->sent_since_poll, 0)) {
    if (timeout_ms > 0)
      atomic_fetch_add(&f->post_send_waits, 1u);
    atomic_fetch_add(&f->post_send_polls, 1u);
  } else if (timeout_ms > 0) {
    atomic_fetch_add(&f->idle_waits, 1u);
  }
}

h2_pal_result_t h2_peer_portable_poll(h2_pal_webrtc_peer_t *peer,
                                      int timeout_ms) {
  fixture_t *f = protocol(peer);
  record_transport_wait(f, timeout_ms);
  atomic_fetch_add(&f->polls, 1u);
  while (atomic_load(&f->poll_gate)) {
    atomic_store(&f->poll_entered, 1);
    (void)h2_pal_time_sleep_ms(f->config.time, 1u);
  }
  h2_pal_result_t rc = atomic_load(&f->poll_result);
  if (rc != H2_PAL_OK)
    return rc;
  if (!peer->production_sctp_open) {
    peer->production_sctp_open = 1;
    h2_peer_webrtc_emit_peer_state(peer, H2_PAL_WEBRTC_PEER_CONNECTED);
    atomic_fetch_add(&f->connected, 1u);
  }
  h2_pal_sctp_stream_reset_event_t reset;
  while (h2_pal_queue_recv(f->config.queue, f->resets, &reset, 0u) ==
         H2_PAL_OK) {
    h2_peer_webrtc_on_stream_reset(peer, &reset);
    atomic_fetch_add(&f->processed, 1u);
  }
  int remote = atomic_exchange(&f->remote_sid, 0);
  if (remote != 0) {
    atomic_store(&f->remote_result,
                 h2_peer_webrtc_on_remote_channel_open(
                     peer, (h2_pal_webrtc_str_t){"remote", 6u},
                     (uint16_t)(remote - 1), 1, 1));
    atomic_fetch_add(&f->remote_processed, 1u);
  }
  return H2_PAL_OK;
}

int h2_peer_portable_async_receive_supported(const h2_pal_webrtc_peer_t *peer) {
  assert(in_protocol_task);
  fixture_t *f = peer->owner->config.mem->user;
  return atomic_load(&f->async_receive);
}

int h2_peer_portable_receive_datagram(h2_pal_webrtc_peer_t *peer,
                                      h2_pal_net_addr_t *addr, uint8_t *packet,
                                      size_t cap, uint32_t timeout_ms) {
  (void)addr;
  (void)packet;
  (void)cap;
  fixture_t *f = protocol(peer);
  assert(atomic_load(&f->async_receive));
  record_transport_wait(f, (int)timeout_ms);
  return 0;
}

h2_pal_result_t h2_peer_portable_service_datagram(h2_pal_webrtc_peer_t *peer,
                                                  h2_pal_net_addr_t *addr,
                                                  uint8_t *packet, size_t len) {
  (void)addr;
  (void)packet;
  assert(len == 0u && atomic_load(&protocol(peer)->async_receive));
  return h2_peer_portable_poll(peer, 0);
}

h2_pal_result_t
h2_peer_portable_channel_open(h2_pal_webrtc_channel_t *channel) {
  fixture_t *f = protocol(channel->owner);
  if (atomic_exchange(&f->terminal_next_open, 0)) {
    const h2_pal_sctp_stream_reset_event_t reset = {
        .stream_id = channel->info.stream_id,
        .direction = H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET,
        .result = H2_PAL_OK,
    };
    h2_peer_webrtc_on_stream_reset(channel->owner, &reset);
  } else if (atomic_load(&f->open_result) == H2_PAL_OK) {
    channel->open = 1;
    h2_peer_webrtc_emit_channel_state(channel, H2_PAL_WEBRTC_CHANNEL_OPEN);
  }
  atomic_fetch_add(&f->opens, 1u);
  return atomic_load(&f->open_result);
}

h2_pal_result_t h2_peer_portable_sctp_is_writable(h2_pal_webrtc_peer_t *peer,
                                                  bool *out) {
  (void)protocol(peer);
  *out = true;
  return H2_PAL_OK;
}

h2_pal_result_t h2_peer_portable_channel_send(h2_pal_webrtc_channel_t *channel,
                                              const uint8_t *data, size_t len,
                                              int text) {
  fixture_t *f = protocol(channel->owner);
  if (atomic_load(&f->send_busy))
    return H2_PAL_ERR_WOULD_BLOCK;
  h2_pal_result_t rc = h2_peer_webrtc_emit_channel_message(
      channel->owner, channel, data, len, text);
  if (rc == H2_PAL_OK) {
    atomic_store(&f->sent_since_poll, 1);
    atomic_fetch_add(&f->sends, 1u);
  }
  return rc;
}

h2_pal_result_t h2_peer_portable_send_opus(h2_pal_webrtc_peer_t *peer,
                                           const uint8_t *opus, size_t len) {
  fixture_t *f = protocol(peer);
  if (atomic_load(&f->opus_busy))
    return H2_PAL_ERR_WOULD_BLOCK;
  h2_peer_webrtc_emit_opus_frame(peer, opus, len);
  atomic_store(&f->sent_since_poll, 1);
  atomic_fetch_add(&f->opus_sends, 1u);
  return H2_PAL_OK;
}

h2_pal_result_t h2_peer_portable_reset_stream(h2_pal_webrtc_peer_t *peer,
                                              uint16_t sid) {
  fixture_t *f = protocol(peer);
  assert(sid < H2_PEER_STREAM_COUNT);
  atomic_fetch_add(&f->reset_attempts, 1u);
  h2_pal_result_t rc = atomic_load(&f->reset_result);
  if (rc == H2_PAL_OK)
    atomic_fetch_add(&f->submitted, 1u);
  return rc;
}

h2_pal_result_t h2_peer_portable_forget_stream(h2_pal_webrtc_peer_t *peer,
                                               uint16_t sid) {
  fixture_t *f = protocol(peer);
  assert(sid < H2_PEER_STREAM_COUNT);
  atomic_fetch_add(&f->forgotten, 1u);
  return H2_PAL_OK;
}

void h2_peer_portable_peer_close(h2_pal_webrtc_peer_t *peer) {
  (void)protocol(peer);
  peer->production_pc = NULL;
  peer->production_sctp_open = 0;
}

static void initialize(fixture_t *f) {
  *f = (fixture_t){0};
  static const h2_pal_mem_vtable_t memory = {.alloc = allocate,
                                             .free = deallocate};
  static const h2_pal_task_vtable_t tasks = {.start = start_task,
                                             .join = join_task};
  f->mem = (h2_pal_mem_api_t){f, &memory};
  f->task = (h2_pal_task_api_t){f, &tasks};
  f->config = (h2_peer_config_t){
      .mem = &f->mem,
      .log = h2_desktop_platform_log_api(),
      .net = h2_pal_unsupported_net_api(),
      .queue = h2_desktop_platform_queue_api(),
      .sync = h2_desktop_platform_sync_api(),
      .task = &f->task,
      .time = h2_desktop_platform_time_api(),
      .crypto = h2_pal_unsupported_crypto_api(),
      .dtls = h2_pal_unsupported_dtls_api(),
      .sctp = h2_pal_unsupported_sctp_api(),
  };
  const h2_pal_queue_config_t reset_queue = {
      .name = "test/protocol/reset",
      .item_size = sizeof(h2_pal_sctp_stream_reset_event_t),
      .item_count = 16u,
      .allocator = h2_desktop_platform_default_allocator(),
  };
  assert(h2_pal_queue_create(f->config.queue, &reset_queue, &f->resets) ==
         H2_PAL_OK);
}

static void create(fixture_t *f) {
  initialize(f);
  assert(h2_peer_create(&f->config, &f->owner) == H2_PAL_OK);
  f->api = h2_peer_webrtc_api(f->owner);
  assert(h2_pal_webrtc_peer_create(f->api, &f->peer) == H2_PAL_OK);
}

static void cleanup(fixture_t *f) {
  h2_peer_destroy(&f->owner);
  assert(f->owner == NULL);
  assert(atomic_load(&f->starts) == atomic_load(&f->joins));
  assert(atomic_load(&f->allocations) == atomic_load(&f->frees));
  h2_pal_queue_destroy(f->config.queue, f->resets);
}

static void wait_count(fixture_t *f, atomic_uint *count, unsigned expected) {
  for (unsigned i = 0u; i < 2000u && atomic_load(count) < expected; ++i)
    (void)h2_pal_time_sleep_ms(f->config.time, 1u);
  assert(atomic_load(count) >= expected);
}

static void drain(fixture_t *f) {
  h2_pal_webrtc_event_t event = {0};
  while (h2_pal_webrtc_peer_poll(f->api, f->peer, 0, &event) == H2_PAL_OK)
    h2_pal_webrtc_event_release(&event);
}

static h2_pal_webrtc_event_t next_kind(fixture_t *f,
                                       h2_pal_webrtc_event_kind_t kind) {
  h2_pal_webrtc_event_t event = {0};
  for (unsigned i = 0u; i < 200u; ++i) {
    h2_pal_result_t rc = h2_pal_webrtc_peer_poll(f->api, f->peer, 10, &event);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      continue;
    assert(rc == H2_PAL_OK);
    if (event.kind == kind)
      return event;
    assert(event.kind != H2_PAL_WEBRTC_EVENT_ERROR);
    h2_pal_webrtc_event_release(&event);
  }
  assert(0 && "expected owned event");
  return event;
}

static void connect(fixture_t *f, unsigned channels) {
  assert(h2_pal_webrtc_peer_start_offer(f->api, f->peer) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_start_offer(f->api, f->peer) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_webrtc_peer_set_remote_sdp(
             f->api, f->peer, H2_PAL_WEBRTC_SDP_ANSWER,
             (h2_pal_webrtc_str_t){"answer", 6u}) == H2_PAL_OK);
  wait_count(f, &f->connected, 1u);
  wait_count(f, &f->opens, channels);
  // Protocol and channel opening completed without application poll.
  drain(f);
}

static h2_pal_result_t open_sid(fixture_t *f, uint16_t sid,
                                h2_pal_webrtc_channel_t **out) {
  const h2_pal_webrtc_channel_config_t config = {
      .label = {"channel", 7u},
      .has_stream_id = 1,
      .stream_id = sid,
      .ordered = 1,
      .reliable = 1,
  };
  return h2_pal_webrtc_peer_create_data_channel(f->api, f->peer, &config, out);
}

static void inject_reset(fixture_t *f, uint16_t sid,
                         h2_pal_sctp_stream_reset_direction_t direction) {
  unsigned processed = atomic_load(&f->processed);
  const h2_pal_sctp_stream_reset_event_t event = {
      .stream_id = sid,
      .direction = direction,
      .result = H2_PAL_OK,
  };
  assert(h2_pal_queue_send(f->config.queue, f->resets, &event, 0u) ==
         H2_PAL_OK);
  wait_count(f, &f->processed, processed + 1u);
}

static void test_pool_and_event_lease(void) {
  fixture_t f;
  create(&f);
  h2_pal_webrtc_channel_t *channels[H2_PEER_READY_CHANNEL_COUNT] = {0};
  for (size_t i = 0u; i < H2_PEER_READY_CHANNEL_COUNT; ++i)
    assert(open_sid(&f, (uint16_t)(i * 2u + 1u), &channels[i]) == H2_PAL_OK);
  h2_pal_webrtc_channel_t *extra = NULL;
  assert(open_sid(&f, 299u, &extra) == H2_PAL_ERR_NO_SPACE && extra == NULL);
  assert(open_sid(&f, 0u, &extra) == H2_PAL_ERR_INVALID_ARG);
  h2_pal_webrtc_channel_close(f.api, channels[0]);
  h2_pal_webrtc_event_t closed =
      next_kind(&f, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE);
  assert(closed.channel_state == H2_PAL_WEBRTC_CHANNEL_CLOSED);
  assert(open_sid(&f, 1u, &channels[0]) == H2_PAL_OK);
  connect(&f, H2_PEER_READY_CHANNEL_COUNT);
  atomic_store(&f.poll_gate, 1);
  for (unsigned i = 0u; i < 2000u && !atomic_load(&f.poll_entered); ++i)
    (void)h2_pal_time_sleep_ms(f.config.time, 1u);
  assert(atomic_load(&f.poll_entered));
  const uint8_t payload[] = {0u, 0x80u, 0xffu};
  assert(h2_pal_webrtc_channel_send(f.api, channels[0], payload,
                                    sizeof(payload), 0) == H2_PAL_OK);
  uint32_t ready = atomic_load(&f.peer->channel_ready);
  assert(ready != 0u);
  // Releasing the old channel lease must not clear the replacement's ready bit.
  h2_pal_webrtc_event_release(&closed);
  assert(atomic_load(&f.peer->channel_ready) == ready);
  atomic_store(&f.poll_gate, 0);
  wait_count(&f, &f.sends, 1u);
  h2_pal_webrtc_event_t message =
      next_kind(&f, H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE);
  assert(message.data_len == sizeof(payload) &&
         memcmp(message.data, payload, sizeof(payload)) == 0);
  h2_pal_webrtc_peer_close(f.api, f.peer);
  h2_peer_destroy(&f.owner);
  assert(message.data_len == sizeof(payload) &&
         memcmp(message.data, payload, sizeof(payload)) == 0);
  h2_pal_webrtc_event_release(&message);
  cleanup(&f);
}

static void test_reset_quarantine(void) {
  fixture_t f;
  create(&f);
  h2_pal_webrtc_channel_t *a, *b, *replacement = NULL;
  assert(open_sid(&f, 1u, &a) == H2_PAL_OK);
  assert(open_sid(&f, 3u, &b) == H2_PAL_OK);
  connect(&f, 2u);
  h2_pal_webrtc_channel_close(f.api, a);
  h2_pal_webrtc_channel_close(f.api, b);
  assert(atomic_load(&f.submitted) == 1u);
  assert(open_sid(&f, 1u, &replacement) == H2_PAL_ERR_INVALID_ARG);
  inject_reset(&f, 1u, H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET);
  assert(open_sid(&f, 1u, &replacement) == H2_PAL_ERR_INVALID_ARG);
  inject_reset(&f, 1u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED);
  wait_count(&f, &f.submitted, 2u);
  assert(open_sid(&f, 1u, &replacement) == H2_PAL_OK);
  inject_reset(&f, 1u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED);
  assert(open_sid(&f, 3u, &b) == H2_PAL_ERR_INVALID_ARG);
  inject_reset(&f, 3u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED);
  assert(open_sid(&f, 3u, &b) == H2_PAL_ERR_INVALID_ARG);
  inject_reset(&f, 3u, H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET);
  wait_count(&f, &f.forgotten, 2u);
  assert(open_sid(&f, 3u, &b) == H2_PAL_OK);
  drain(&f);
  cleanup(&f);
}

static void test_reset_busy_then_failure(void) {
  fixture_t f;
  create(&f);
  h2_pal_webrtc_channel_t *a, *b;
  assert(open_sid(&f, 1u, &a) == H2_PAL_OK);
  assert(open_sid(&f, 3u, &b) == H2_PAL_OK);
  connect(&f, 2u);
  atomic_store(&f.reset_result, H2_PAL_ERR_WOULD_BLOCK);
  h2_pal_webrtc_channel_close(f.api, a);
  wait_count(&f, &f.reset_attempts, 2u);
  assert(atomic_load(&f.submitted) == 0u);
  atomic_store(&f.reset_result, H2_PAL_ERR_IO);
  h2_pal_webrtc_event_t error = next_kind(&f, H2_PAL_WEBRTC_EVENT_ERROR);
  assert(error.error == H2_PAL_ERR_IO);
  h2_pal_webrtc_event_release(&error);
  cleanup(&f);
}

static void test_terminal_during_open(void) {
  for (unsigned fails = 0u; fails < 2u; ++fails) {
    fixture_t f;
    create(&f);
    connect(&f, 0u);
    atomic_store(&f.terminal_next_open, 1);
    atomic_store(&f.open_result, fails ? H2_PAL_ERR_IO : H2_PAL_OK);
    h2_pal_webrtc_channel_t *channel = (void *)(uintptr_t)1u;
    assert(open_sid(&f, 1u, &channel) ==
           (fails ? H2_PAL_ERR_IO : H2_PAL_ERR_CLOSED));
    assert(channel == NULL);
    cleanup(&f);
  }
}

static h2_pal_result_t read_track(void *user, uint8_t *out, size_t cap,
                                  size_t *len) {
  fixture_t *f = user;
  assert(in_protocol_task);
  if (!atomic_exchange(&f->track_ready, 0))
    return H2_PAL_ERR_WOULD_BLOCK;
  assert(cap >= 2u);
  out[0] = 0xf8u;
  out[1] = 0x42u;
  *len = 2u;
  atomic_fetch_add(&f->track_reads, 1u);
  return H2_PAL_OK;
}

static h2_pal_result_t write_track(void *user, const uint8_t *data,
                                   size_t len) {
  fixture_t *f = user;
  assert(in_protocol_task && len == 2u && data[0] == 0xf8u && data[1] == 0x42u);
  atomic_fetch_add(&f->track_writes, 1u);
  return H2_PAL_OK;
}

static void test_media_and_channel_busy(void) {
  fixture_t f;
  create(&f);
  static const h2_pal_webrtc_track_vtable_t vtable = {.read = read_track,
                                                      .write = write_track};
  h2_pal_webrtc_track_t track = {.user = &f, .vtable = &vtable};
  assert(h2_pal_webrtc_peer_set_track(f.api, f.peer, &track) == H2_PAL_OK);
  h2_pal_webrtc_channel_t *channel = NULL;
  assert(open_sid(&f, 1u, &channel) == H2_PAL_OK);
  connect(&f, 1u);
  atomic_store(&f.opus_busy, 1);
  atomic_store(&f.send_busy, 1);
  atomic_store(&f.track_ready, 1);
  wait_count(&f, &f.track_reads, 1u);
  const uint8_t message[] = {0u, 0xffu, 0x42u};
  // A busy transport still accepts one ring's worth of queued messages.
  for (size_t i = 0u; i < H2_PEER_INPUT_SLOT_COUNT; ++i) {
    assert(h2_pal_webrtc_channel_send(f.api, channel, message,
                                      sizeof(message), 0) == H2_PAL_OK);
  }
  assert(h2_pal_webrtc_channel_send(f.api, channel, message, sizeof(message),
                                    0) == H2_PAL_ERR_WOULD_BLOCK);
  assert(atomic_load(&f.track_reads) == 1u && atomic_load(&f.opus_sends) == 0u);
  atomic_store(&f.opus_busy, 0);
  atomic_store(&f.send_busy, 0);
  wait_count(&f, &f.track_writes, 1u);
  wait_count(&f, &f.sends, H2_PEER_INPUT_SLOT_COUNT);
  assert(atomic_load(&f.track_reads) == 1u);
  for (size_t i = 0u; i < H2_PEER_INPUT_SLOT_COUNT; ++i) {
    h2_pal_webrtc_event_t event =
        next_kind(&f, H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE);
    assert(event.data_len == sizeof(message) &&
           memcmp(event.data, message, sizeof(message)) == 0);
    h2_pal_webrtc_event_release(&event);
  }
  assert(h2_pal_webrtc_peer_unset_track(f.api, f.peer, &track) == H2_PAL_OK);
  track.vtable = NULL;
  cleanup(&f);
}

static void test_send_progress_does_not_idle_wait(int async_receive) {
  fixture_t f;
  create(&f);
  atomic_store(&f.async_receive, async_receive);
  h2_pal_webrtc_channel_t *channel = NULL;
  assert(open_sid(&f, 1u, &channel) == H2_PAL_OK);
  connect(&f, 1u);
  // Idle peers must still use bounded waiting rather than busy-spin.
  wait_count(&f, &f.idle_waits, 1u);
  const uint8_t message[] = {0x42u};
  atomic_store(&f.send_busy, 1);
  assert(h2_pal_webrtc_channel_send(f.api, channel, message, sizeof(message),
                                    0) == H2_PAL_OK);
  // A blocked slot is not progress: keep the data and use idle waiting.
  wait_count(&f, &f.idle_waits, atomic_load(&f.idle_waits) + 10u);
  assert(atomic_load(&f.sends) == 0u);
  atomic_store(&f.send_busy, 0);
  wait_count(&f, &f.post_send_polls, 1u);
  assert(atomic_load(&f.post_send_waits) == 0u);
  drain(&f);
  assert(h2_pal_webrtc_peer_send_opus(f.api, f.peer, message,
                                      sizeof(message)) == H2_PAL_OK);
  wait_count(&f, &f.post_send_polls, 2u);
  assert(atomic_load(&f.post_send_waits) == 0u);
  cleanup(&f);
}

static void test_allocations_and_config(void) {
  for (size_t fail = 1u; fail < 24u; ++fail) {
    fixture_t f;
    initialize(&f);
    atomic_store(&f.fail_at, fail);
    h2_pal_result_t rc = h2_peer_create(&f.config, &f.owner);
    if (rc == H2_PAL_OK) {
      f.api = h2_peer_webrtc_api(f.owner);
      rc = h2_pal_webrtc_peer_create(f.api, &f.peer);
      if (rc == H2_PAL_OK) {
        h2_pal_webrtc_channel_t *channel = NULL;
        rc = open_sid(&f, 1u, &channel);
        assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_NO_MEMORY);
        const h2_pal_webrtc_ice_server_t server = {
            .url = {"stun:example.invalid:3478", 25u}};
        rc = h2_pal_webrtc_peer_add_ice_server(f.api, f.peer, &server);
        assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_NO_MEMORY);
      } else {
        assert(rc == H2_PAL_ERR_NO_MEMORY && f.peer == NULL);
      }
    } else {
      assert(rc == H2_PAL_ERR_NO_MEMORY && f.owner == NULL);
    }
    cleanup(&f);
  }
  fixture_t f;
  initialize(&f);
  h2_pal_net_vtable_t net = *f.config.net->vtable;
  net.tcp_send_timeout = NULL;
  h2_pal_net_api_t net_api = {NULL, &net};
  f.config.net = &net_api;
  assert(h2_peer_create(&f.config, &f.owner) == H2_PAL_ERR_UNSUPPORTED);
  f.config.net = h2_pal_unsupported_net_api();
  h2_pal_time_vtable_t time = *f.config.time->vtable;
  time.get_monotonic_us = NULL;
  h2_pal_time_api_t time_api = {NULL, &time};
  f.config.time = &time_api;
  assert(h2_peer_create(&f.config, &f.owner) == H2_PAL_ERR_INVALID_ARG);
  f.config.time = h2_desktop_platform_time_api();
  const h2_pal_log_vtable_t log = {0};
  const h2_pal_log_api_t log_api = {NULL, &log};
  f.config.log = &log_api;
  assert(h2_peer_create(&f.config, &f.owner) == H2_PAL_ERR_INVALID_ARG);
  cleanup(&f);
}

static h2_pal_result_t remote_open(fixture_t *f, uint16_t sid) {
  unsigned processed = atomic_load(&f->remote_processed);
  atomic_store(&f->remote_sid, (int)sid + 1);
  wait_count(f, &f->remote_processed, processed + 1u);
  return atomic_load(&f->remote_result);
}

static void test_remote_lifecycle(void) {
  fixture_t f;
  create(&f);
  connect(&f, 0u);
  assert(remote_open(&f, 0u) == H2_PAL_OK);
  h2_pal_webrtc_event_t old = next_kind(&f, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE);
  assert(old.channel_state == H2_PAL_WEBRTC_CHANNEL_OPEN);
  assert(old.channel_info.stream_id == 0u && old.channel_info.label.len == 6u &&
         memcmp(old.channel_info.label.data, "remote", 6u) == 0);
  assert(remote_open(&f, 0u) == H2_PAL_ERR_INVALID_ARG);
  assert(remote_open(&f, 1u) == H2_PAL_ERR_INVALID_ARG);
  assert(remote_open(&f, H2_PEER_STREAM_COUNT) == H2_PAL_ERR_INVALID_ARG);
  h2_pal_webrtc_channel_close(f.api, old.channel);
  h2_pal_webrtc_event_t closed =
      next_kind(&f, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE);
  assert(closed.channel == old.channel &&
         closed.channel_state == H2_PAL_WEBRTC_CHANNEL_CLOSED);
  h2_pal_webrtc_event_release(&closed);
  assert(remote_open(&f, 0u) == H2_PAL_ERR_INVALID_ARG);
  inject_reset(&f, 0u, H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET);
  inject_reset(&f, 0u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED);
  wait_count(&f, &f.forgotten, 1u);
  assert(remote_open(&f, 0u) == H2_PAL_OK);
  h2_pal_webrtc_event_t replacement =
      next_kind(&f, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE);
  assert(replacement.channel_state == H2_PAL_WEBRTC_CHANNEL_OPEN &&
         replacement.channel != old.channel);
  h2_pal_webrtc_event_release(&old);
  assert(h2_pal_webrtc_channel_send(f.api, replacement.channel,
                                    (const uint8_t *)"echo", 4u,
                                    1) == H2_PAL_OK);
  wait_count(&f, &f.sends, 1u);
  h2_pal_webrtc_event_t message =
      next_kind(&f, H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE);
  assert(message.channel == replacement.channel && message.is_text &&
         message.data_len == 4u && memcmp(message.data, "echo", 4u) == 0);
  h2_pal_webrtc_event_release(&message);
  h2_pal_webrtc_event_release(&replacement);
  cleanup(&f);
}

static void test_offer_answer_and_transport_errors(void) {
  fixture_t f;
  create(&f);
  const h2_pal_webrtc_ice_server_t server = {
      .url = {"stun:example.invalid:3478",
              sizeof("stun:example.invalid:3478") - 1u}};
  assert(h2_pal_webrtc_peer_add_ice_server(f.api, f.peer, &server) ==
         H2_PAL_OK);
  atomic_store(&f.offer_result, H2_PAL_ERR_IO);
  assert(h2_pal_webrtc_peer_start_offer(f.api, f.peer) == H2_PAL_ERR_IO);
  atomic_store(&f.offer_result, H2_PAL_OK);
  assert(h2_pal_webrtc_peer_start_offer(f.api, f.peer) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_add_ice_server(f.api, f.peer, &server) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_pal_webrtc_event_t offer = next_kind(&f, H2_PAL_WEBRTC_EVENT_LOCAL_SDP);
  assert(offer.sdp_type == H2_PAL_WEBRTC_SDP_OFFER && offer.data_len == 5u &&
         memcmp(offer.data, "offer", 5u) == 0);
  h2_pal_webrtc_event_release(&offer);
  assert(h2_pal_webrtc_peer_set_remote_sdp(
             f.api, f.peer, H2_PAL_WEBRTC_SDP_OFFER,
             (h2_pal_webrtc_str_t){"answer", 6u}) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_webrtc_peer_set_remote_sdp(
             f.api, f.peer, H2_PAL_WEBRTC_SDP_ANSWER,
             (h2_pal_webrtc_str_t){"bad", 3u}) == H2_PAL_ERR_FORMAT);
  atomic_store(&f.answer_result, H2_PAL_ERR_IO);
  assert(h2_pal_webrtc_peer_set_remote_sdp(
             f.api, f.peer, H2_PAL_WEBRTC_SDP_ANSWER,
             (h2_pal_webrtc_str_t){"answer", 6u}) == H2_PAL_ERR_IO);
  atomic_store(&f.answer_result, H2_PAL_OK);
  assert(h2_pal_webrtc_peer_set_remote_sdp(
             f.api, f.peer, H2_PAL_WEBRTC_SDP_ANSWER,
             (h2_pal_webrtc_str_t){"answer", 6u}) == H2_PAL_OK);
  wait_count(&f, &f.connected, 1u);
  atomic_store(&f.poll_result, H2_PAL_ERR_IO);
  h2_pal_webrtc_event_t error = next_kind(&f, H2_PAL_WEBRTC_EVENT_ERROR);
  assert(error.error == H2_PAL_ERR_IO);
  h2_pal_webrtc_event_release(&error);
  cleanup(&f);
}

static void test_destroy_retries_failed_join(void) {
  for (unsigned explicit_close = 0u; explicit_close < 2u; ++explicit_close) {
    fixture_t f;
    create(&f);
    h2_peer_t *owner = f.owner;
    atomic_store(&f.fail_joins, 1);
    if (explicit_close) {
      h2_pal_webrtc_peer_close(f.api, f.peer);
    } else {
      h2_peer_destroy(&f.owner);
      assert(f.owner == owner && h2_peer_webrtc_api(f.owner) == NULL);
      h2_pal_webrtc_peer_t *rejected = NULL;
      assert(h2_pal_webrtc_peer_create(f.api, &rejected) == H2_PAL_ERR_CLOSED);
      assert(rejected == NULL);
    }
    assert(atomic_load(&f.joins) == 0u);
    assert(owner->peers == f.peer);
    cleanup(&f);
  }
}

typedef struct release_call {
  fixture_t *fixture;
  atomic_int *gate;
  h2_pal_webrtc_event_t events[2];
} release_call_t;

static void release_events(void *user) {
  release_call_t *call = user;
  while (!atomic_load(call->gate))
    (void)h2_pal_time_sleep_ms(call->fixture->config.time, 1u);
  for (size_t i = 0u; i < 2u; ++i) {
    assert(call->events[i].data_len == 4u &&
           memcmp(call->events[i].data, "echo", 4u) == 0);
    h2_pal_webrtc_event_release(&call->events[i]);
  }
}

static void test_concurrent_event_release_and_destroy(void) {
  for (unsigned round = 0u; round < 8u; ++round) {
    fixture_t f;
    create(&f);
    h2_pal_webrtc_peer_t *peers[2] = {f.peer, NULL};
    assert(h2_pal_webrtc_peer_create(f.api, &peers[1]) == H2_PAL_OK);
    atomic_int gate = 0;
    release_call_t calls[4] = {0};
    h2_pal_task_t *workers[4] = {0};
    for (size_t p = 0u; p < 2u; ++p) {
      f.peer = peers[p];
      h2_pal_webrtc_channel_t *channel = NULL;
      assert(open_sid(&f, 1u, &channel) == H2_PAL_OK);
      unsigned connected = atomic_load(&f.connected);
      unsigned opens = atomic_load(&f.opens);
      assert(h2_pal_webrtc_peer_start_offer(f.api, f.peer) == H2_PAL_OK);
      assert(h2_pal_webrtc_peer_set_remote_sdp(
                 f.api, f.peer, H2_PAL_WEBRTC_SDP_ANSWER,
                 (h2_pal_webrtc_str_t){"answer", 6u}) == H2_PAL_OK);
      wait_count(&f, &f.connected, connected + 1u);
      wait_count(&f, &f.opens, opens + 1u);
      for (size_t i = 0u; i < 4u; ++i) {
        h2_pal_result_t rc = H2_PAL_ERR_WOULD_BLOCK;
        for (unsigned retry = 0u; retry < 2000u && rc == H2_PAL_ERR_WOULD_BLOCK;
             ++retry) {
          rc = h2_pal_webrtc_channel_send(f.api, channel,
                                          (const uint8_t *)"echo", 4u, 1);
          if (rc == H2_PAL_ERR_WOULD_BLOCK)
            (void)h2_pal_time_sleep_ms(f.config.time, 1u);
        }
        assert(rc == H2_PAL_OK);
        size_t n = p * 2u + i / 2u;
        calls[n].fixture = &f;
        calls[n].gate = &gate;
        calls[n].events[i % 2u] =
            next_kind(&f, H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE);
      }
    }
    for (size_t i = 0u; i < 4u; ++i) {
      const h2_pal_task_options_t options = {.name = "test/event-release",
                                             .min_stack_size = 64u * 1024u};
      assert(h2_pal_task_start(f.config.task, &options, release_events,
                               &calls[i], &workers[i]) == H2_PAL_OK);
    }
    atomic_store(&gate, 1);
    h2_peer_destroy(&f.owner);
    assert(f.owner == NULL);
    for (size_t i = 0u; i < 4u; ++i)
      assert(h2_pal_task_join(f.config.task, workers[i]) == H2_PAL_OK);
    cleanup(&f);
  }
}

static void test_ice_server_transport_validation(void) {
  fixture_t f;
  create(&f);
  h2_pal_webrtc_ice_server_t server = {.username = {"user", 4u},
                                       .credential = {"test-only", 9u}};
  const struct {
    const char *url;
    h2_pal_result_t result;
  } cases[] = {
      {"turns:example.invalid:5349", H2_PAL_ERR_UNSUPPORTED},
      {"turn:example.invalid:3478?transport=tcp", H2_PAL_ERR_UNSUPPORTED},
      {"http:example.invalid", H2_PAL_ERR_FORMAT},
      {"turn:example.invalid:3478?transport=udp", H2_PAL_OK},
  };
  for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    server.url = (h2_pal_webrtc_str_t){cases[i].url, strlen(cases[i].url)};
    assert(h2_pal_webrtc_peer_add_ice_server(f.api, f.peer, &server) ==
           cases[i].result);
  }
  server.credential = (h2_pal_webrtc_str_t){0};
  assert(h2_pal_webrtc_peer_add_ice_server(f.api, f.peer, &server) ==
         H2_PAL_ERR_INVALID_ARG);
  cleanup(&f);
}

static void test_terminal_while_opening_pending_channel(void) {
  fixture_t f;
  create(&f);
  h2_pal_webrtc_channel_t *channel = NULL;
  assert(open_sid(&f, 1u, &channel) == H2_PAL_OK);
  atomic_store(&f.terminal_next_open, 1);
  assert(h2_pal_webrtc_peer_start_offer(f.api, f.peer) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_set_remote_sdp(
             f.api, f.peer, H2_PAL_WEBRTC_SDP_ANSWER,
             (h2_pal_webrtc_str_t){"answer", 6u}) == H2_PAL_OK);
  h2_pal_webrtc_event_t closed =
      next_kind(&f, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE);
  assert(closed.channel == channel &&
         closed.channel_state == H2_PAL_WEBRTC_CHANNEL_CLOSED);
  h2_pal_webrtc_event_release(&closed);
  h2_pal_webrtc_event_t error = next_kind(&f, H2_PAL_WEBRTC_EVENT_ERROR);
  assert(error.error == H2_PAL_ERR_CLOSED && atomic_load(&f.opens) == 1u);
  h2_pal_webrtc_event_release(&error);
  cleanup(&f);
}

typedef struct send_call {
  fixture_t *fixture;
  h2_pal_webrtc_channel_t *channel;
  atomic_int go;
  h2_pal_result_t result;
} send_call_t;

static void send_while_closing(void *user) {
  send_call_t *call = user;
  while (!atomic_load(&call->go))
    (void)h2_pal_time_sleep_ms(call->fixture->config.time, 1u);
  call->result = h2_pal_webrtc_channel_send(call->fixture->api, call->channel,
                                            (const uint8_t *)"echo", 4u, 1);
}

static void test_remote_reset_during_send(void) {
  for (unsigned round = 0u; round < 16u; ++round) {
    fixture_t f;
    create(&f);
    connect(&f, 0u);
    assert(remote_open(&f, 0u) == H2_PAL_OK);
    h2_pal_webrtc_event_t opened =
        next_kind(&f, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE);
    assert(opened.channel_state == H2_PAL_WEBRTC_CHANNEL_OPEN);
    send_call_t call = {.fixture = &f, .channel = opened.channel};
    atomic_init(&call.go, 0);
    h2_pal_task_t *sender = NULL;
    const h2_pal_task_options_t options = {.name = "test/send-reset",
                                           .min_stack_size = 64u * 1024u};
    assert(h2_pal_task_start(f.config.task, &options, send_while_closing, &call,
                             &sender) == H2_PAL_OK);
    atomic_store(&call.go, 1);
    inject_reset(&f, 0u, H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET);
    assert(h2_pal_task_join(f.config.task, sender) == H2_PAL_OK);
    assert(call.result == H2_PAL_OK || call.result == H2_PAL_ERR_INVALID_STATE);
    h2_pal_webrtc_event_t closed =
        next_kind(&f, H2_PAL_WEBRTC_EVENT_CHANNEL_STATE);
    assert(closed.channel == opened.channel &&
           closed.channel_state == H2_PAL_WEBRTC_CHANNEL_CLOSED);
    h2_pal_webrtc_event_release(&closed);
    // Retain the opening lease until the sender exits and terminal event is
    // consumed.
    h2_pal_webrtc_event_release(&opened);
    cleanup(&f);
  }
}

int main(void) {
  test_pool_and_event_lease();
  test_reset_quarantine();
  test_reset_busy_then_failure();
  test_terminal_during_open();
  test_media_and_channel_busy();
  test_send_progress_does_not_idle_wait(0);
  test_send_progress_does_not_idle_wait(1);
  test_allocations_and_config();
  test_remote_lifecycle();
  test_offer_answer_and_transport_errors();
  test_destroy_retries_failed_join();
  test_concurrent_event_release_and_destroy();
  test_ice_server_transport_validation();
  test_terminal_while_opening_pending_channel();
  test_remote_reset_during_send();
  return 0;
}
