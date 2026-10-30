#include "h2_peer.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_desktop_platform.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

namespace {
const h2_pal_queue_api_t* real_queue;
const h2_pal_time_api_t* real_time;
h2_pal_queue_t* events;
thread_local bool observing = false;
uint64_t now_us;
std::vector<uint32_t> waits;

int create_queue(void* user, const h2_pal_queue_config_t* config,
                 h2_pal_queue_t** out) {
  const int rc = real_queue->vtable->create(user, config, out);
  if (rc == H2_PAL_OK && !std::strcmp(config->name, "h2peer/net/events"))
    events = *out;
  return rc;
}

int receive(void* user, h2_pal_queue_t* queue, void* item, uint32_t timeout) {
  if (observing && queue == events) {
    // An empty queue can wake early. The public poll must still request a
    // positive blocking wait while its deadline has any positive time left.
    waits.push_back(timeout);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  return real_queue->vtable->recv(user, queue, item, timeout);
}

int monotonic_us(void* user, uint64_t* out) {
  if (observing) {
    *out = now_us;
    now_us += 100u;
    return H2_PAL_OK;
  }
  return real_time->vtable->get_monotonic_us(user, out);
}
}  // namespace

int main() {
  real_queue = h2_desktop_platform_queue_api();
  real_time = h2_desktop_platform_time_api();
  auto queue_ops = *real_queue->vtable;
  queue_ops.create = create_queue;
  queue_ops.recv = receive;
  const h2_pal_queue_api_t queue = {real_queue->user, &queue_ops};
  auto time_ops = *real_time->vtable;
  time_ops.get_monotonic_us = monotonic_us;
  const h2_pal_time_api_t time = {real_time->user, &time_ops};
  h2_peer_config_t config = {};
  config.mem = h2_desktop_platform_default_allocator();
  config.log = h2_desktop_platform_log_api();
  config.queue = &queue;
  config.task = h2_desktop_platform_task_api();
  config.time = &time;
  config.sync = h2_desktop_platform_sync_api();
  config.net = h2_pal_unsupported_net_api();
  config.crypto = h2_pal_unsupported_crypto_api();
  config.dtls = h2_pal_unsupported_dtls_api();
  config.sctp = h2_pal_unsupported_sctp_api();
  h2_peer_t* owner = nullptr;
  assert(h2_peer_create(&config, &owner) == H2_PAL_OK);
  const auto* api = h2_peer_webrtc_api(owner);
  h2_pal_webrtc_peer_t* peer = nullptr;
  assert(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK);
  assert(events != nullptr);
  for (int timeout : {1, 41}) {
    waits.clear();
    now_us = 1000000u;
    observing = true;
    h2_pal_webrtc_event_t event = {};
    const int rc = h2_pal_webrtc_peer_poll(api, peer, timeout, &event);
    observing = false;
    assert(rc == H2_PAL_ERR_TIMEOUT);
    assert(waits.size() > 2u && waits.front() == H2_PAL_QUEUE_NO_WAIT);
    assert(std::all_of(waits.begin() + 1, waits.end(),
                       [](uint32_t ms) { return ms > 0u && ms <= 20u; }));
  }
  h2_pal_webrtc_peer_close(api, peer);
  h2_peer_destroy(&owner);
  assert(owner == nullptr);
}
