#include "h2_peer.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_desktop_platform.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>

// Exercise the production worker under a continuously ready public command.
// Only the command queue boundary is replayed; no transport is fabricated.
namespace {
const h2_pal_queue_api_t* real_queue;
const h2_pal_task_api_t* real_task;
const h2_pal_time_api_t* real_time;
h2_pal_queue_t *commands, *responses;
size_t command_size;
alignas(uint64_t) unsigned char saved_command[256];
std::atomic<bool> record{false}, busy{false};
std::atomic<unsigned> processed{0}, cooperative_waits{0};
thread_local bool network_thread = false;
h2_pal_task_entry_t network_entry;
void* network_context;
int create_queue(void* user, const h2_pal_queue_config_t* config,
                 h2_pal_queue_t** out) {
  int rc = real_queue->vtable->create(user, config, out);
  if (!rc && !std::strcmp(config->name, "h2peer/net/commands")) {
    commands = *out;
    command_size = config->item_size;
    assert(command_size <= sizeof(saved_command));
  }
  if (!rc && !std::strcmp(config->name, "h2peer/net/responses"))
    responses = *out;
  return rc;
}
int send(void* user, h2_pal_queue_t* queue, const void* item,
         uint32_t timeout) {
  if (queue == commands && record.load())
    std::memcpy(saved_command, item, command_size);
  if (queue == responses && busy.load()) return H2_PAL_OK;
  return real_queue->vtable->send(user, queue, item, timeout);
}
int recv(void* user, h2_pal_queue_t* queue, void* item, uint32_t timeout) {
  if (queue == commands && busy.load()) {
    if (processed.load() < 100u) {
      std::memcpy(item, saved_command, command_size);
      processed.fetch_add(1u);
      return H2_PAL_OK;
    }
    busy.store(false);
  }
  return real_queue->vtable->recv(user, queue, item, timeout);
}
void run_network(void*) {
  network_thread = true;
  network_entry(network_context);
}
int start(void* user, const h2_pal_task_options_t* options,
          h2_pal_task_entry_t entry, void* context, h2_pal_task_t** out) {
  if (!std::strcmp(options->name, "$h2peer/net")) {
    network_entry = entry;
    network_context = context;
    return real_task->vtable->start(user, options, run_network, nullptr, out);
  }
  return real_task->vtable->start(user, options, entry, context, out);
}
int sleep_ms(void* user, uint32_t ms) {
  if (network_thread && busy.load() && processed.load() && ms)
    cooperative_waits.fetch_add(1u);
  return real_time->vtable->sleep_ms(user, ms);
}
h2_pal_result_t read_track(void*, uint8_t*, size_t, size_t*) {
  assert(false);
  return H2_PAL_ERR_UNSUPPORTED;
}
}  // namespace
int main() {
  real_queue = h2_desktop_platform_queue_api();
  real_task = h2_desktop_platform_task_api();
  real_time = h2_desktop_platform_time_api();
  auto qops = *real_queue->vtable;
  qops.create = create_queue;
  qops.send = send;
  qops.recv = recv;
  const h2_pal_queue_api_t queue = {real_queue->user, &qops};
  auto tops = *real_task->vtable;
  tops.start = start;
  const h2_pal_task_api_t task = {real_task->user, &tops};
  auto clock = *real_time->vtable;
  clock.sleep_ms = sleep_ms;
  const h2_pal_time_api_t time = {real_time->user, &clock};
  h2_peer_config_t config = {};
  config.mem = h2_desktop_platform_default_allocator();
  config.log = h2_desktop_platform_log_api();
  config.queue = &queue;
  config.task = &task;
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
  h2_pal_webrtc_track_vtable_t track_ops = {};
  track_ops.read = read_track;
  h2_pal_webrtc_track_t track = {};
  track.vtable = &track_ops;
  record.store(true);
  assert(h2_pal_webrtc_peer_set_track(api, peer, &track) == H2_PAL_OK);
  record.store(false);
  assert(commands && responses && command_size);
  busy.store(true);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (busy.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(!busy.load() && processed.load() == 100u);
  // An idle sleep after the backlog ends cannot satisfy this observation.
  assert(cooperative_waits.load() > 0u);
  h2_pal_webrtc_peer_close(api, peer);
  h2_peer_destroy(&owner);
  assert(owner == nullptr);
}
