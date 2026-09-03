#include "h2/pal/h2_pal_unsupported.h"
#include "h2_desktop_platform.h"
#include "h2_peer.h"

#include <atomic>
// These tests use assertions for both checks and the operations under test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace {
const h2_pal_sync_api_t *real_sync;
const h2_pal_task_api_t *real_task;
h2_pal_mutex_t *request_mutex;
std::mutex gate_mutex;
std::condition_variable gate_changed;
bool parked = false;
bool resume = false;
thread_local bool park_before_lock = false;
std::atomic<unsigned> started{0}, joined{0};
bool fail_start = false;

h2_pal_result_t create_mutex(void *user, const h2_pal_mutex_config_t *config,
                             h2_pal_mutex_t **out) {
  const auto rc = real_sync->vtable->create_mutex(user, config, out);
  if (rc == H2_PAL_OK && std::strcmp(config->name, "h2peer/net/request") == 0)
    request_mutex = *out;
  return rc;
}

h2_pal_result_t lock_mutex(void *user, h2_pal_mutex_t *mutex) {
  if (park_before_lock && mutex == request_mutex) {
    std::unique_lock<std::mutex> lock(gate_mutex);
    parked = true;
    gate_changed.notify_all();
    assert(gate_changed.wait_for(lock, std::chrono::seconds(5),
                                 [] { return resume; }));
  }
  return real_sync->vtable->lock_mutex(user, mutex);
}

int start_task(void *user, const h2_pal_task_options_t *options,
               h2_pal_task_entry_t entry, void *context, h2_pal_task_t **out) {
  if (fail_start) {
    *out = nullptr;
    return H2_PAL_ERR_UNSUPPORTED;
  }
  const auto rc = real_task->vtable->start(user, options, entry, context, out);
  if (rc == H2_PAL_OK)
    ++started;
  return rc;
}

int join_task(void *user, h2_pal_task_t *task) {
  const auto rc = real_task->vtable->join(user, task);
  if (rc == H2_PAL_OK)
    ++joined;
  return rc;
}

h2_pal_result_t read_track(void *, uint8_t *, size_t, size_t *) {
  // No offer/connection in this control-only test. No media may be consumed.
  assert(false);
  return H2_PAL_ERR_UNSUPPORTED;
}
} // namespace

int main() {
  real_sync = h2_desktop_platform_sync_api();
  real_task = h2_desktop_platform_task_api();
  auto sync_vtable = *real_sync->vtable;
  sync_vtable.create_mutex = create_mutex;
  sync_vtable.lock_mutex = lock_mutex;
  const h2_pal_sync_api_t sync = {real_sync->user, &sync_vtable};
  auto task_vtable = *real_task->vtable;
  task_vtable.start = start_task;
  task_vtable.join = join_task;
  const h2_pal_task_api_t task = {real_task->user, &task_vtable};
  h2_peer_config_t config = {};
  config.mem = h2_desktop_platform_default_allocator();
  config.log = h2_desktop_platform_log_api();
  config.net = h2_pal_unsupported_net_api();
  config.queue = h2_desktop_platform_queue_api();
  config.sync = &sync;
  config.task = &task;
  config.time = h2_desktop_platform_time_api();
  config.crypto = h2_pal_unsupported_crypto_api();
  config.dtls = h2_pal_unsupported_dtls_api();
  config.sctp = h2_pal_unsupported_sctp_api();
  h2_peer_t *owner = nullptr;
  // Public constructor with the real task and command queue.
  // Transport is deliberately unavailable: this test must not contact a server.
  assert(h2_peer_create(&config, &owner) == H2_PAL_OK);
  const auto *api = h2_peer_webrtc_api(owner);
  h2_pal_webrtc_peer_t *peer = nullptr;
  fail_start = true;
  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_ERR_UNSUPPORTED);
  assert(peer == nullptr && started == 0u);
  fail_start = false;
  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK);
  assert(started == 1u);

  h2_pal_webrtc_track_vtable_t track_vtable = {};
  track_vtable.read = read_track;
  h2_pal_webrtc_track_t a = {}, b = {};
  a.vtable = b.vtable = &track_vtable;
  assert(h2_pal_webrtc_peer_set_track(api, peer, &a) == H2_PAL_OK);
  int stale_result = H2_PAL_OK;
  std::thread stale_unset([&] {
    park_before_lock = true;
    stale_result = h2_pal_webrtc_peer_unset_track(api, peer, &a);
  });
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    assert(gate_changed.wait_for(lock, std::chrono::seconds(5),
                                 [] { return parked; }));
  }
  // Replace A while its older unset caller has not yet acquired the control
  // lock.
  assert(h2_pal_webrtc_peer_unset_track(api, peer, &a) == H2_PAL_OK);
  assert(h2_pal_webrtc_peer_set_track(api, peer, &b) == H2_PAL_OK);
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    resume = true;
    gate_changed.notify_all();
  }
  stale_unset.join();
  assert(stale_result == H2_PAL_ERR_INVALID_STATE);
  // The stale command must leave B attached; neither caller needs app poll.
  assert(h2_pal_webrtc_peer_unset_track(api, peer, &b) == H2_PAL_OK);
  h2_pal_webrtc_event_t event = {};
  assert(h2_pal_webrtc_peer_poll(api, peer, -1, &event) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_webrtc_peer_poll(api, peer, 0, &event) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(event._private == nullptr);

  // A second caller must not consume events concurrently, and a close must
  // wait for the in-flight poll instead of destroying the queue under it.
  std::atomic<int> blocked_result{H2_PAL_OK};
  std::atomic<bool> blocked_entered{false};
  std::atomic<int> blocked_kind{0};
  std::thread blocked_poll([&] {
    h2_pal_webrtc_event_t blocked_event = {};
    int rc = H2_PAL_ERR_BUSY;
    // The prober below can hold the slot for the length of one non-blocking
    // poll, so retry rather than mistaking that for the contended case.
    while (rc == H2_PAL_ERR_BUSY) {
      blocked_entered = true;
      rc = h2_pal_webrtc_peer_poll(api, peer, 5000, &blocked_event);
    }
    blocked_kind = static_cast<int>(blocked_event.kind);
    blocked_result = rc;
    h2_pal_webrtc_event_release(&blocked_event);
  });
  while (!blocked_entered)
    std::this_thread::yield();
  h2_pal_webrtc_event_t busy_event = {};
  int busy_result = H2_PAL_OK;
  const auto busy_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    busy_result = h2_pal_webrtc_peer_poll(api, peer, 0, &busy_event);
    if (busy_result == H2_PAL_OK)
      h2_pal_webrtc_event_release(&busy_event);
    if (busy_result != H2_PAL_ERR_BUSY)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (busy_result != H2_PAL_ERR_BUSY &&
           std::chrono::steady_clock::now() < busy_deadline);
  assert(busy_result == H2_PAL_ERR_BUSY);
  // Closing here tears down the queue this poll is parked on. It must wait for
  // the poll to leave, and the poll must end on a terminal result rather than
  // a freed queue: either the terminal peer-state event or CLOSED.
  h2_pal_webrtc_peer_close(api, peer);
  blocked_poll.join();
  assert(blocked_result == H2_PAL_ERR_CLOSED ||
         (blocked_result == H2_PAL_OK &&
          blocked_kind == H2_PAL_WEBRTC_EVENT_PEER_STATE));
  h2_peer_destroy(&owner);
  assert(owner == nullptr && started == 1u && joined == 1u);
  return 0;
}
