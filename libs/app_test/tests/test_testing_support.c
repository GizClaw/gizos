#include "h2_app_test_mem.h"
#include "h2_app_test_sync.h"
#include "h2_app_test_task.h"
#include "h2_app_test_time.h"
#include "h2_app_test_webrtc.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct backend {
  h2_pal_webrtc_api_t api;
  unsigned creates, closes, polls, releases, observations;
  int peer_token, channel_token;
  h2_pal_result_t poll_result, send_result;
} backend_t;
static backend_t *active_backend;
static h2_pal_result_t create_peer(void *user, h2_pal_webrtc_peer_t **out) {
  backend_t *b = user;
  ++b->creates;
  *out = (h2_pal_webrtc_peer_t *)&b->peer_token;
  return H2_PAL_OK;
}
static void close_peer(h2_pal_webrtc_peer_t *peer) {
  backend_t *b = active_backend;
  assert(peer == (h2_pal_webrtc_peer_t *)&b->peer_token);
  ++b->closes;
}
static void release_event(h2_pal_webrtc_event_t *event) {
  backend_t *b = event->_private;
  /* The observer must restore the original event, including its raw handles. */
  assert(event->peer == (h2_pal_webrtc_peer_t *)&b->peer_token);
  assert(event->channel == (h2_pal_webrtc_channel_t *)&b->channel_token);
  assert(event->_release == release_event);
  ++b->releases;
  memset(event, 0, sizeof(*event));
}
static h2_pal_result_t poll_peer(h2_pal_webrtc_peer_t *peer, int timeout,
                                 h2_pal_webrtc_event_t *out) {
  backend_t *b = active_backend;
  assert(peer == (h2_pal_webrtc_peer_t *)&b->peer_token && timeout == 7);
  ++b->polls;
  if (b->poll_result)
    return b->poll_result;
  *out = (h2_pal_webrtc_event_t){
      .kind = H2_PAL_WEBRTC_EVENT_CHANNEL_STATE,
      .peer = peer,
      .channel = (h2_pal_webrtc_channel_t *)&b->channel_token,
      .channel_state = H2_PAL_WEBRTC_CHANNEL_OPEN,
      .channel_info = {.label = {"test", 4},
                       .has_stream_id = 1,
                       .stream_id = 3},
      ._private = b,
      ._release = release_event};
  return H2_PAL_OK;
}
static h2_pal_result_t send_channel(h2_pal_webrtc_channel_t *channel,
                                    const uint8_t *bytes, size_t size,
                                    int text) {
  backend_t *b = active_backend;
  assert(channel == (h2_pal_webrtc_channel_t *)&b->channel_token);
  assert(size == 1 && bytes[0] == 42 && !text);
  return b->send_result;
}
static void observe(void *user, const h2_pal_webrtc_event_t *event) {
  backend_t *b = user;
  assert(event->peer != (h2_pal_webrtc_peer_t *)&b->peer_token);
  assert(event->channel != (h2_pal_webrtc_channel_t *)&b->channel_token);
  assert(event->channel_info.stream_id == 3);
  ++b->observations;
}
static const h2_pal_webrtc_vtable_t backend_vtable = {
    .peer_create = create_peer,
    .peer_close = close_peer,
    .peer_poll = poll_peer,
    .channel_send = send_channel};
static void increment(void *user) { ++*(unsigned *)user; }
int main(void) {
  h2_app_test_mem_t mem;
  h2_app_test_mem_init(&mem, NULL);
  uint8_t *data = h2_pal_mem_alloc(&mem.api, 19);
#if defined(_MSC_VER) && !defined(__clang__)
  assert(data && (uintptr_t)data % __alignof(long double) == 0);
#else
  assert(data && (uintptr_t)data % _Alignof(max_align_t) == 0);
#endif
  memset(data, 0x42, 19);
  mem.fail_at = mem.calls + 1;
  assert(!h2_pal_mem_realloc(&mem.api, data, 37));
  assert(mem.live_blocks == 1 && mem.live_bytes == 19 && data[18] == 0x42);
  data = h2_pal_mem_realloc(&mem.api, data, 37);
  assert(data && data[18] == 0x42 && mem.live_blocks == 1 &&
         mem.live_bytes == 37);
  h2_pal_mem_free(&mem.api, data);
  assert(mem.live_blocks == 0 && mem.live_bytes == 0);
  assert(!h2_pal_mem_alloc(&mem.api, SIZE_MAX));

  h2_app_test_task_t task;
  h2_app_test_task_init(&task);
  unsigned ran = 0;
  h2_pal_task_t *handle = NULL;
  h2_pal_task_options_t options = {.name = "test"};
  task.start = (h2_app_test_fault_t){.result = H2_PAL_ERR_TASK, .remaining = 1};
  assert(h2_pal_task_start(&task.api, &options, increment, &ran, &handle) ==
         H2_PAL_ERR_TASK);
  assert(!handle && !task.active && !ran);
  assert(h2_pal_task_start(&task.api, &options, increment, &ran, &handle) ==
         H2_PAL_OK);
  assert(h2_pal_task_join(&task.api, handle) == H2_PAL_ERR_BUSY);
  assert(h2_app_test_task_run(&task) == H2_PAL_OK && ran == 1);
  task.join = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1};
  assert(h2_pal_task_join(&task.api, handle) == H2_PAL_ERR_IO && task.active);
  assert(h2_pal_task_join(&task.api, handle) == H2_PAL_OK && !task.active);

  h2_app_test_sync_t sync;
  h2_app_test_sync_init(&sync);
  h2_pal_mutex_t *mutex = NULL;
  h2_pal_mutex_config_t config = {.name = "test", .allocator = &mem.api};
  assert(h2_pal_mutex_create(&sync.api, &config, &mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_lock(&sync.api, mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_try_lock(&sync.api, mutex) == H2_PAL_ERR_BUSY);
  assert(h2_pal_mutex_destroy(&sync.api, mutex) == H2_PAL_ERR_BUSY);
  assert(h2_pal_mutex_unlock(&sync.api, mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_destroy(&sync.api, mutex) == H2_PAL_OK);

  backend_t backends[2] = {0};
  h2_app_test_webrtc_t *wrappers[2] = {0};
  h2_pal_webrtc_peer_t *peers[2] = {0};
  for (unsigned i = 0; i < 2; ++i) {
    backend_t *b = &backends[i];
    active_backend = b;
    b->api = (h2_pal_webrtc_api_t){b, &backend_vtable};
    assert(h2_app_test_webrtc_create(&mem.api, &b->api, observe, b,
                                     &wrappers[i]) == H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_app_test_webrtc_api(wrappers[i]);
    assert(h2_pal_webrtc_peer_create(api, &peers[i]) == H2_PAL_OK);
    assert(h2_app_test_webrtc_destroy(wrappers[i]) == H2_PAL_ERR_INVALID_STATE);
    h2_pal_webrtc_event_t event = {0};
    mem.fail_at = mem.calls + 1;
    assert(h2_pal_webrtc_peer_poll(api, peers[i], 7, &event) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(!b->polls);
    assert(h2_pal_webrtc_peer_poll(api, peers[i], 7, &event) == H2_PAL_OK);
    assert(b->observations == 1 && !b->releases);
    uint8_t byte = 42;
    b->send_result = H2_PAL_ERR_WOULD_BLOCK;
    assert(h2_pal_webrtc_channel_send(api, event.channel, &byte, 1, 0) ==
           H2_PAL_ERR_WOULD_BLOCK);
    h2_pal_webrtc_channel_t *channel = event.channel;
    h2_pal_webrtc_event_release(&event);
    assert(b->releases == 1 && !event._private);
    assert(h2_pal_webrtc_peer_poll(api, peers[i], 7, &event) == H2_PAL_OK);
    assert(event.channel == channel);
    h2_pal_webrtc_event_release(&event);
    b->poll_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_pal_webrtc_peer_poll(api, peers[i], 7, &event) ==
           H2_PAL_ERR_TIMEOUT);
  }
  for (unsigned i = 0; i < 2; ++i) {
    active_backend = &backends[i];
    h2_pal_webrtc_peer_close(h2_app_test_webrtc_api(wrappers[i]), peers[i]);
    assert(backends[i].closes == 1);
    assert(h2_app_test_webrtc_destroy(wrappers[i]) == H2_PAL_OK);
  }
  assert(mem.live_blocks == 0);
  assert(h2_pal_mem_alloc(&mem.api, 8));
  assert(h2_pal_mem_alloc(&mem.api, 16));
  h2_app_test_mem_release_all(&mem);
  assert(mem.live_blocks == 0 && mem.live_bytes == 0);
  return 0;
}
