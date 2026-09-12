/* Downlink stream observation: fake Peer Events go through the real client
 * dispatch on the Service network task, and boundaries come out of
 * service_poll() on this thread. */

#include "events/peer_event.pb.h"
#include "gzc_common.h"
#include "h2_desktop_platform.h"
#include "h2_gizclaw_conversation.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_service_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { AUDIO = gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO };
enum { TEXT = gizclaw_events_v1_StreamKind_STREAM_KIND_TEXT };
#define BEGIN H2_GIZCLAW_DOWNLINK_STREAM_BEGIN
#define END H2_GIZCLAW_DOWNLINK_STREAM_END

/* Scripted Event stream read by the network task. */
typedef struct {
  pthread_mutex_t lock;
  gzc_peer_event_t script[64];
  size_t head, tail;
  unsigned idle_reads;
  bool closed;
} feed_t;
static feed_t s_feed = {.lock = PTHREAD_MUTEX_INITIALIZER};

typedef struct {
  h2_gizclaw_downlink_stream_event_t events[32];
  size_t count;
  pthread_t poll_thread;
} recorder_t;

static void sleep_ms(uint32_t ms) {
  h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), ms);
}

static int feed_read(void *user, gzc_event_stream_t *stream, int timeout,
                     gzc_peer_event_t *event) {
  (void)user;
  (void)stream;
  (void)timeout;
  int rc = GZC_ERR_WOULD_BLOCK;
  pthread_mutex_lock(&s_feed.lock);
  if (s_feed.closed) {
    rc = GZC_ERR_CLOSED;
  } else if (s_feed.head != s_feed.tail) {
    *event = s_feed.script[s_feed.head++];
    s_feed.idle_reads = 0u;
    rc = GZC_OK;
  } else {
    ++s_feed.idle_reads;
  }
  pthread_mutex_unlock(&s_feed.lock);
  return rc;
}

static void feed_close(void *user, gzc_event_stream_t *stream) {
  (void)user;
  (void)stream;
}

static void feed(gzc_peer_event_t event) {
  pthread_mutex_lock(&s_feed.lock);
  assert(s_feed.tail < sizeof(s_feed.script) / sizeof(s_feed.script[0]));
  s_feed.script[s_feed.tail++] = event;
  pthread_mutex_unlock(&s_feed.lock);
}

/* Wait until the network task has handled every scripted event: it reads
 * again only after the previous event's callbacks returned. */
static void feed_drain(void) {
  for (;;) {
    pthread_mutex_lock(&s_feed.lock);
    const bool drained =
        s_feed.head == s_feed.tail && s_feed.idle_reads >= 2u;
    pthread_mutex_unlock(&s_feed.lock);
    if (drained)
      return;
    sleep_ms(1u);
  }
}

static void feed_reset(void) {
  pthread_mutex_lock(&s_feed.lock);
  s_feed.head = s_feed.tail = 0u;
  s_feed.idle_reads = 0u;
  s_feed.closed = false;
  pthread_mutex_unlock(&s_feed.lock);
}

static gzc_peer_event_t bos(int kind, const char *label, const char *id) {
  gzc_peer_event_t event = gizclaw_events_v1_PeerEvent_init_zero;
  event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS;
  event.which_payload = gizclaw_events_v1_PeerEvent_bos_tag;
  event.payload.bos.kind = kind;
  (void)snprintf(event.payload.bos.label, sizeof(event.payload.bos.label), "%s",
                 label);
  (void)snprintf(event.payload.bos.stream_id,
                 sizeof(event.payload.bos.stream_id), "%s", id);
  return event;
}

static gzc_peer_event_t eos(int kind, const char *label, const char *id,
                            const char *error) {
  gzc_peer_event_t event = gizclaw_events_v1_PeerEvent_init_zero;
  event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS;
  event.which_payload = gizclaw_events_v1_PeerEvent_eos_tag;
  event.payload.eos.kind = kind;
  (void)snprintf(event.payload.eos.label, sizeof(event.payload.eos.label), "%s",
                 label);
  (void)snprintf(event.payload.eos.stream_id,
                 sizeof(event.payload.eos.stream_id), "%s", id);
  if (error != NULL) {
    event.payload.eos.has_error = true;
    (void)snprintf(event.payload.eos.error.code,
                   sizeof(event.payload.eos.error.code), "%s", error);
  }
  return event;
}

static void record(void *user, const h2_gizclaw_downlink_stream_event_t *event) {
  recorder_t *rec = user;
  assert(pthread_equal(pthread_self(), rec->poll_thread));
  assert(rec->count < sizeof(rec->events) / sizeof(rec->events[0]));
  rec->events[rec->count++] = *event;
}

static h2_pal_result_t fake_connect(h2_gizclaw_client_t *client) {
  (void)h2_gizclaw_test_replace_event_stream(client,
                                             (gzc_event_stream_t *)&s_feed);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_poll(h2_gizclaw_client_t *client, int timeout) {
  (void)client;
  (void)timeout;
  sleep_ms(1u);
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t real_dispatch(h2_gizclaw_client_t *client) {
  return (h2_pal_result_t)h2_gizclaw_client_dispatch_event(client, 0, NULL,
                                                           NULL);
}

static const h2_gizclaw_service_client_ops_t s_ops = {
    .connect = fake_connect, .poll = fake_poll, .dispatch_event = real_dispatch};

static h2_gizclaw_service_t *make_service(recorder_t *rec) {
  static h2_gizclaw_config_t client_config;
  static const h2_pal_http_api_t http = {0};
  static const h2_pal_crypto_api_t crypto = {0};
  static const h2_pal_webrtc_api_t webrtc = {0};
  memset(&client_config, 0, sizeof(client_config));
  client_config.allocator = h2_desktop_platform_default_allocator();
  client_config.time = h2_desktop_platform_time_api();
  client_config.http = &http;
  client_config.crypto = &crypto;
  client_config.webrtc = &webrtc;
  client_config.connect_timeout_ms = 1000;
  client_config.server_endpoint = (h2_gizclaw_str_t){"127.0.0.1:1", 11u};
  client_config.private_key = (h2_gizclaw_str_t){"test-key", 8u};
  const h2_gizclaw_service_config_t config = {
      .client_config = &client_config,
      .task = h2_desktop_platform_task_api(),
      .queue = h2_desktop_platform_queue_api(),
      .sync = h2_desktop_platform_sync_api(),
      .net_task_options = {.name = "downlink-stream-test"},
      .operation_capacity = 4u,
      .client_poll_timeout_ms = 1,
      .on_downlink_stream = rec != NULL ? record : NULL,
      .downlink_stream_user = rec,
  };
  if (rec != NULL) {
    memset(rec, 0, sizeof(*rec));
    rec->poll_thread = pthread_self();
  }
  feed_reset();
  h2_gizclaw_service_test_set_client_ops(&s_ops);
  h2_gizclaw_test_set_event_ops(NULL, feed_read, feed_close, NULL);
  h2_gizclaw_service_t *service = NULL;
  assert(h2_gizclaw_service_init(&config, &service) == H2_PAL_OK);
  assert((service->downlink_streams != NULL) == (rec != NULL));
  return service;
}

static void finish_service(h2_gizclaw_service_t *service) {
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  size_t dispatched = 0u;
  do
    assert(h2_gizclaw_service_poll(service, 8u, &dispatched) == H2_PAL_OK);
  while (dispatched != 0u);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  h2_gizclaw_test_set_event_ops(NULL, NULL, NULL, NULL);
  h2_gizclaw_service_test_set_client_ops(NULL);
}

/* Poll until `count` boundaries arrived, then check no more follow. */
static void poll_expect(h2_gizclaw_service_t *service, recorder_t *rec,
                        size_t count) {
  for (unsigned i = 0u; i < 5000u && rec->count < count; ++i) {
    assert(h2_gizclaw_service_poll(service, 8u, NULL) == H2_PAL_OK);
    if (rec->count < count)
      sleep_ms(1u);
  }
  for (unsigned i = 0u; i < 5u; ++i)
    assert(h2_gizclaw_service_poll(service, 8u, NULL) == H2_PAL_OK);
  assert(rec->count == count);
}

static void expect(const recorder_t *rec, size_t index,
                   h2_gizclaw_downlink_stream_kind_t kind, const char *id,
                   const char *label, bool interrupted) {
  assert(index < rec->count);
  const h2_gizclaw_downlink_stream_event_t *event = &rec->events[index];
  assert(event->kind == kind);
  assert(strcmp(event->stream_id, id) == 0);
  assert(strcmp(event->label, label) == 0);
  assert(event->interrupted == interrupted);
}

/* Boundaries come out in order. Text streams, our own input and a stream
 * that is not open are ignored; an EOS error code marks the end
 * interrupted; a stream without a label reports an empty one. */
static void test_begin_end_ordering(void) {
  recorder_t rec;
  h2_gizclaw_service_t *service = make_service(&rec);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  feed(bos(TEXT, "transcript", "text-1"));
  feed(bos(AUDIO, "demo-home", "demo-1"));
  feed(eos(AUDIO, "assistant", "reply-0", NULL));
  feed(bos(AUDIO, "assistant", "reply-1"));
  feed(bos(AUDIO, "assistant", "reply-1"));
  feed(eos(TEXT, "assistant", "reply-1", NULL));
  feed(eos(0, "assistant", "reply-1", NULL));
  feed(eos(AUDIO, "assistant", "reply-1", NULL));
  feed(eos(AUDIO, "assistant", "reply-1", NULL));
  feed(bos(AUDIO, "", "reply-2"));
  feed(eos(AUDIO, "", "reply-2", "STREAM_INTERRUPTED"));
  feed_drain();
  poll_expect(service, &rec, 4u);
  expect(&rec, 0u, BEGIN, "reply-1", "assistant", false);
  expect(&rec, 1u, END, "reply-1", "assistant", false);
  expect(&rec, 2u, BEGIN, "reply-2", "", false);
  expect(&rec, 3u, END, "reply-2", "", true);
  finish_service(service);
  assert(rec.count == 4u);
}

/* Remote speakers take turns: a new BOS before the previous EOS ends the
 * previous stream, whose late EOS is then ignored. */
static void test_interleaved_speakers(void) {
  recorder_t rec;
  h2_gizclaw_service_t *service = make_service(&rec);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  feed(bos(AUDIO, "peer-a", "sfu/peer-a/1"));
  feed(bos(AUDIO, "peer-b", "sfu/peer-b/2"));
  feed(eos(AUDIO, "peer-a", "sfu/peer-a/1", NULL));
  feed(eos(AUDIO, "peer-b", "sfu/peer-b/2", NULL));
  feed(bos(AUDIO, "peer-a", "sfu/peer-a/3"));
  feed(eos(AUDIO, "peer-a", "sfu/peer-a/3", NULL));
  feed_drain();
  poll_expect(service, &rec, 6u);
  expect(&rec, 0u, BEGIN, "sfu/peer-a/1", "peer-a", false);
  expect(&rec, 1u, END, "sfu/peer-a/1", "peer-a", true);
  expect(&rec, 2u, BEGIN, "sfu/peer-b/2", "peer-b", false);
  expect(&rec, 3u, END, "sfu/peer-b/2", "peer-b", false);
  expect(&rec, 4u, BEGIN, "sfu/peer-a/3", "peer-a", false);
  expect(&rec, 5u, END, "sfu/peer-a/3", "peer-a", false);
  finish_service(service);
}

/* A stream with no EOS stays open across a Conversation release, because
 * its audio keeps playing, and ends interrupted when the connection's
 * events close. Playback is the same with and without the hook. */
static void run_missing_eos(bool hook) {
  recorder_t rec;
  h2_gizclaw_service_t *service = make_service(hook ? &rec : NULL);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_conversation_t *conversation = NULL;
  assert(h2_gizclaw_conversation_create(
             service, (h2_gizclaw_str_t){"workspace", 9u}, NULL, NULL, NULL,
             &conversation) == H2_PAL_OK);
  const uint8_t packet[3] = {0xf8, 0xff, 0xfe};
  h2_gizclaw_conversation_downlink_hold_internal(service);
  assert(h2_gizclaw_service_media_write_opus(service, packet, sizeof(packet)) ==
         H2_PAL_OK);
  assert(h2_gizclaw_test_downlink_frames(service) == 0u);
  feed(bos(AUDIO, "peer-a", "sfu/peer-a/1"));
  feed_drain();
  /* The BOS released the hold; nothing was polled yet. */
  assert(h2_gizclaw_service_media_write_opus(service, packet, sizeof(packet)) ==
         H2_PAL_OK);
  assert(h2_gizclaw_test_downlink_frames(service) == 1u);
  h2_gizclaw_conversation_release(conversation);
  if (hook) {
    poll_expect(service, &rec, 1u);
    expect(&rec, 0u, BEGIN, "sfu/peer-a/1", "peer-a", false);
  }
  pthread_mutex_lock(&s_feed.lock);
  s_feed.closed = true;
  pthread_mutex_unlock(&s_feed.lock);
  if (hook) {
    poll_expect(service, &rec, 2u);
    expect(&rec, 1u, END, "sfu/peer-a/1", "peer-a", true);
  } else {
    for (unsigned i = 0u; i < 20u; ++i) {
      size_t dispatched = 0u;
      assert(h2_gizclaw_service_poll(service, 8u, &dispatched) == H2_PAL_OK);
      assert(dispatched == 0u);
      sleep_ms(1u);
    }
  }
  finish_service(service);
  if (hook)
    assert(rec.count == 2u);
}

static void test_missing_eos(void) { run_missing_eos(true); }

static void test_no_hook_default(void) { run_missing_eos(false); }

/* Fields at their wire maximum are reported whole. */
static void test_maximum_fields(void) {
  char id[H2_GIZCLAW_DOWNLINK_STREAM_ID_MAX_BYTES + 1u];
  char label[H2_GIZCLAW_DOWNLINK_STREAM_LABEL_MAX_BYTES + 1u];
  memset(id, 'i', sizeof(id) - 1u);
  id[sizeof(id) - 1u] = '\0';
  memset(label, 'l', sizeof(label) - 1u);
  label[sizeof(label) - 1u] = '\0';
  gzc_peer_event_t begin = bos(AUDIO, label, id);
  assert(strlen(begin.payload.bos.stream_id) == sizeof(id) - 1u);
  assert(strlen(begin.payload.bos.label) == sizeof(label) - 1u);
  recorder_t rec;
  h2_gizclaw_service_t *service = make_service(&rec);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  feed(begin);
  feed(eos(AUDIO, label, id, NULL));
  feed_drain();
  poll_expect(service, &rec, 2u);
  expect(&rec, 0u, BEGIN, id, label, false);
  expect(&rec, 1u, END, id, label, false);
  finish_service(service);
}

static void boundary(h2_gizclaw_service_t *service, bool begin,
                     const char *id, size_t id_size, const char *label,
                     size_t label_size) {
  const h2_gizclaw_downlink_boundary_t value = {
      .begin = begin,
      .stream_id = id,
      .stream_id_size = id_size,
      .label = label,
      .label_size = label_size,
  };
  h2_gizclaw_downlink_stream_boundary_internal(service, &value);
}

static void pair(h2_gizclaw_service_t *service, const char *id) {
  boundary(service, true, id, strlen(id) + 1u, "peer", 5u);
  boundary(service, false, id, strlen(id) + 1u, "peer", 5u);
}

/* Longer fields than the wire allows, from a future SDK, are not truncated:
 * a cut identity could name someone else. The BOS is not reported but still
 * ends the open stream. An empty or unterminated stream ID is rejected too.
 * Runs on a Service that is not started, so this thread is the only one
 * producing boundaries. */
static void test_oversized_fields_rejected(void) {
  char long_label[100];
  memset(long_label, 'l', sizeof(long_label) - 1u);
  long_label[sizeof(long_label) - 1u] = '\0';
  char long_id[200];
  memset(long_id, 'i', sizeof(long_id) - 1u);
  long_id[sizeof(long_id) - 1u] = '\0';
  const char unterminated[4] = {'a', 'b', 'c', 'd'};
  recorder_t rec;
  h2_gizclaw_service_t *service = make_service(&rec);
  boundary(service, true, "s-1", 4u, "peer", 5u);
  boundary(service, true, "s-2", 4u, long_label, sizeof(long_label));
  boundary(service, false, "s-2", 4u, "", 1u);
  boundary(service, true, long_id, sizeof(long_id), "peer", 5u);
  boundary(service, true, "", 1u, "peer", 5u);
  boundary(service, true, unterminated, sizeof(unterminated), "peer", 5u);
  boundary(service, true, "s-3", 4u, unterminated, sizeof(unterminated));
  /* An absent label is empty, not invalid. */
  boundary(service, true, "s-4", 4u, NULL, 0u);
  boundary(service, false, "s-4", 4u, NULL, 0u);
  poll_expect(service, &rec, 4u);
  expect(&rec, 0u, BEGIN, "s-1", "peer", false);
  expect(&rec, 1u, END, "s-1", "peer", true);
  expect(&rec, 2u, BEGIN, "s-4", "", false);
  expect(&rec, 3u, END, "s-4", "", false);
  finish_service(service);
}

/* An App that does not poll loses the oldest whole pairs, never half of
 * one: every END it gets follows its BEGIN. */
static void test_undelivered_overflow_drops_pairs(void) {
  recorder_t rec;
  h2_gizclaw_service_t *service = make_service(&rec);
  char id[8];
  for (unsigned i = 0u; i < 10u; ++i) {
    (void)snprintf(id, sizeof(id), "p-%u", i);
    pair(service, id);
  }
  poll_expect(service, &rec, 8u);
  for (unsigned i = 0u; i < 4u; ++i) {
    (void)snprintf(id, sizeof(id), "p-%u", i + 6u);
    expect(&rec, i * 2u, BEGIN, id, "peer", false);
    expect(&rec, i * 2u + 1u, END, id, "peer", false);
  }
  /* The head is an END whose BEGIN was delivered: the pair after it goes. */
  boundary(service, true, "a", 2u, "peer", 5u);
  poll_expect(service, &rec, 9u);
  boundary(service, false, "a", 2u, "peer", 5u);
  pair(service, "b");
  pair(service, "c");
  pair(service, "d");
  pair(service, "e");
  poll_expect(service, &rec, 16u);
  expect(&rec, 8u, BEGIN, "a", "peer", false);
  expect(&rec, 9u, END, "a", "peer", false);
  expect(&rec, 10u, BEGIN, "c", "peer", false);
  expect(&rec, 11u, END, "c", "peer", false);
  expect(&rec, 14u, BEGIN, "e", "peer", false);
  expect(&rec, 15u, END, "e", "peer", false);
  finish_service(service);
}

int main(void) {
  test_begin_end_ordering();
  test_interleaved_speakers();
  test_missing_eos();
  test_no_hook_default();
  test_maximum_fields();
  test_oversized_fields_rejected();
  test_undelivered_overflow_drops_pairs();
  puts("gizclaw downlink stream tests passed");
  return 0;
}
