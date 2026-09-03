#include "h2_desktop_platform.h"
#include "h2_pion.h"

// Test operations and checks must run in optimized builds too.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_state {
  atomic_size_t track_reads;
} test_state_t;
static atomic_int s_fail_next_alloc;
static atomic_size_t s_allocations;
static const h2_pal_task_api_t *s_real_task;
static int s_fail_start, s_fail_join;
static unsigned s_started, s_joined;

static int start_task(void *user, const h2_pal_task_options_t *options,
                      h2_pal_task_entry_t entry, void *context,
                      h2_pal_task_t **out) {
  if (s_fail_start) {
    *out = NULL;
    return H2_PAL_ERR_TASK;
  }
  int rc = s_real_task->vtable->start(user, options, entry, context, out);
  if (rc == H2_PAL_OK)
    s_started++;
  return rc;
}

static int join_task(void *user, h2_pal_task_t *task) {
  if (s_fail_join)
    return H2_PAL_ERR_TASK;
  int rc = s_real_task->vtable->join(user, task);
  if (rc == H2_PAL_OK)
    s_joined++;
  return rc;
}

static void *test_alloc(void *user, size_t len) {
  (void)user;
  if (atomic_exchange(&s_fail_next_alloc, 0)) {
    return NULL;
  }
  void *ptr = malloc(len);
  if (ptr != NULL)
    s_allocations++;
  return ptr;
}
static void *test_realloc(void *user, void *ptr, size_t len) {
  (void)user;
  // These tests require the provider to preserve ownership across realloc.
  if (ptr == NULL)
    return test_alloc(user, len);
  assert(len != 0u);
  return realloc(ptr, len);
}
static void test_free(void *user, void *ptr) {
  (void)user;
  if (ptr != NULL)
    s_allocations--;
  free(ptr);
}

static h2_pal_result_t failed_read(void *user, uint8_t *opus, size_t capacity,
                                   size_t *out_len) {
  (void)user;
  (void)opus;
  (void)capacity;
  (void)out_len;
  return H2_PAL_ERR_FORMAT;
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

typedef struct blocked_poll_state {
  const h2_pal_webrtc_api_t *api;
  h2_pal_webrtc_peer_t *peer;
  volatile int entered;
  volatile int result;
} blocked_poll_state_t;

static void blocked_poll_entry(void *ctx) {
  blocked_poll_state_t *state = ctx;
  h2_pal_webrtc_event_t event = {0};
  int rc = H2_PAL_ERR_BUSY;
  /* The prober holds the slot for the length of one non-blocking poll, so
   * retry rather than mistaking that for the contended case. */
  while (rc == H2_PAL_ERR_BUSY) {
    state->entered = 1;
    rc = h2_pal_webrtc_peer_poll(state->api, state->peer, 5000, &event);
  }
  state->result = rc;
  h2_pal_webrtc_event_release(&event);
}

int main(void) {
  static const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc, .realloc = test_realloc, .free = test_free};
  h2_pal_mem_api_t mem = {.user = NULL, .vtable = &mem_vtable};
  h2_pion_t *provider = NULL;
  assert(h2_pion_create(NULL, &provider) == H2_PAL_ERR_INVALID_ARG);
  s_real_task = h2_desktop_platform_task_api();
  h2_pal_task_vtable_t task_vtable = *s_real_task->vtable;
  task_vtable.start = start_task;
  task_vtable.join = join_task;
  h2_pal_task_api_t task_api = {.user = s_real_task->user,
                                .vtable = &task_vtable};
  h2_pion_config_t config = {
      .mem = &mem,
      .sync = h2_desktop_platform_sync_api(),
      .task = &task_api,
      .time = h2_desktop_platform_time_api(),
  };
  assert(h2_pion_create(&config, &provider) == H2_PAL_OK);
  const h2_pal_webrtc_api_t *api = h2_pion_webrtc_api(provider);
  assert(api != NULL);

  h2_pal_webrtc_peer_t *peer = NULL;
  size_t baseline = s_allocations;
  s_fail_start = 1;
  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_ERR_TASK);
  assert(peer == NULL && s_started == 0u && s_allocations == baseline);
  s_fail_start = 0;
  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK);
  test_state_t test = {0};
  static const h2_pal_webrtc_track_vtable_t track_vtable = {
      .read = test_track_read};
  h2_pal_webrtc_track_t track = {.user = &test, .vtable = &track_vtable};
  assert(h2_pal_webrtc_peer_set_track(api, peer, &track) == H2_PAL_OK);

  h2_pion_test_block_next_opus_send(peer);
  assert(h2_pion_test_connected(peer) == H2_PAL_OK);
  // No poll: both the initial read and WOULD_BLOCK retry belong to the worker.
  for (unsigned n = 0; n < 5000u && h2_pion_test_opus_send_attempts(peer) < 2u;
       ++n)
    assert(h2_pal_time_sleep_ms(config.time, 1u) == H2_PAL_OK);
  assert(test.track_reads == 1u);
  assert(h2_pion_test_opus_send_attempts(peer) == 2u);
  assert(h2_pion_test_opus_send_payloads_match(peer));
  h2_pal_webrtc_event_t event = {0};
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_PEER_STATE);
  assert(event.peer_state == H2_PAL_WEBRTC_PEER_CONNECTED);
  h2_pal_webrtc_event_release(&event);

  assert(h2_pal_webrtc_peer_unset_track(api, peer, &track) == H2_PAL_OK);

  s_fail_next_alloc = 1;
  assert(h2_pion_test_remote_channel(peer) == H2_PAL_ERR_NO_MEMORY);
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
  /* A second caller must not consume events concurrently, and closing while a
   * poll is parked on the semaphore must wait for it instead of destroying the
   * semaphore, mutex and peer under it. */
  blocked_poll_state_t blocked = {.api = api, .peer = peer};
  const h2_pal_task_options_t blocked_options = {.name = "pion/test/poll"};
  h2_pal_task_t *blocked_task = NULL;
  assert(h2_pal_task_start(&task_api, &blocked_options, blocked_poll_entry,
                           &blocked, &blocked_task) == H2_PAL_OK);
  while (!blocked.entered)
    assert(h2_pal_time_sleep_ms(config.time, 1u) == H2_PAL_OK);
  int busy_result = H2_PAL_OK;
  for (unsigned n = 0; n < 5000u && busy_result != H2_PAL_ERR_BUSY; ++n) {
    h2_pal_webrtc_event_t busy_event = {0};
    busy_result = h2_pal_webrtc_peer_poll(api, peer, 0, &busy_event);
    if (busy_result == H2_PAL_OK)
      h2_pal_webrtc_event_release(&busy_event);
    if (busy_result != H2_PAL_ERR_BUSY)
      assert(h2_pal_time_sleep_ms(config.time, 1u) == H2_PAL_OK);
  }
  assert(busy_result == H2_PAL_ERR_BUSY);
  h2_pal_webrtc_peer_close(api, peer);
  assert(h2_pal_task_join(&task_api, blocked_task) == H2_PAL_OK);
  assert(blocked.result == H2_PAL_ERR_CLOSED ||
         blocked.result == H2_PAL_ERR_TIMEOUT);
  assert(s_started == s_joined);
  h2_pion_destroy(&provider);
  assert(provider == NULL);
  // Owned event payload/metadata remains valid after peer/provider destruction.
  assert(event.channel_info.label.len == 3u);
  assert(memcmp(event.channel_info.label.data, "rpc", 3u) == 0);
  h2_pal_webrtc_event_release(&event);
  assert(s_allocations == 0u);

  assert(h2_pion_create(&config, &provider) == H2_PAL_OK);
  api = h2_pion_webrtc_api(provider);
  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK);
  // The C-facing queue is finite, and saturation rejects rather than consumes
  // an event; the Go Dispatch test verifies that rejection preserves FIFO.
  unsigned enqueued = 0u;
  h2_pal_result_t rc;
  while ((rc = h2_pion_test_remote_channel(peer)) == H2_PAL_OK) {
    assert(++enqueued <= 256u);
  }
  assert(enqueued == 256u && rc == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_OK);
  h2_pal_webrtc_event_release(&event);
  assert(h2_pion_test_remote_channel(peer) == H2_PAL_OK);
  h2_pal_webrtc_peer_close(api, peer);

  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK);
  static const h2_pal_webrtc_track_vtable_t failed_vtable = {.read =
                                                                 failed_read};
  h2_pal_webrtc_track_t failed_track = {.vtable = &failed_vtable};
  assert(h2_pal_webrtc_peer_set_track(api, peer, &failed_track) == H2_PAL_OK);
  assert(h2_pion_test_connected(peer) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_poll(api, peer, 1000, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_PEER_STATE);
  h2_pal_webrtc_event_release(&event);
  assert(h2_pal_webrtc_peer_poll(api, peer, 1000, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_WEBRTC_EVENT_ERROR &&
         event.error == H2_PAL_ERR_FORMAT);
  h2_pal_webrtc_event_release(&event);
  assert(h2_pal_webrtc_peer_unset_track(api, peer, &failed_track) == H2_PAL_OK);

  s_fail_join = 1;
  h2_pion_destroy(&provider);
  assert(provider != NULL && s_started == s_joined + 1u);
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) == H2_PAL_ERR_CLOSED);
  s_fail_join = 0;
  h2_pion_destroy(&provider);
  assert(provider == NULL && s_started == s_joined && s_allocations == 0u);
  return 0;
}
