#include "h2_peer_internal.h"
extern "C" {
#include "providers/h2_peer_portable_backend.h"
}
#include "h2_desktop_platform.h"
#include "h2_pal_unsupported.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>

/* Compile the real network worker, substituting only protocol boundaries.
 * Public APIs construct/control it and enqueue the mid-burst media. */
namespace {
constexpr unsigned kPackets = 16;
constexpr unsigned kArrivalPacket = 4;
constexpr uint64_t kPacketCostUs = 3000;
constexpr uint8_t kOpus = 0x42;
struct Snapshot {
  unsigned packets, receive_calls, send_attempts, successes, sent_packet;
  unsigned channel_messages, channel_bytes, eos_reads;
  uint64_t admitted_us, sent_us;
  bool pending_empty;
};
struct Fixture {
  bool track_source;
  unsigned blocked_attempts;
  const h2_pal_task_api_t* real_task;
  const h2_pal_time_api_t* real_time;
  h2_pal_task_entry_t entry = nullptr;
  void* context = nullptr;
  h2_peer_t* owner = nullptr;
  h2_pal_webrtc_peer_t* peer = nullptr;
  h2_pal_webrtc_channel_t* channel = nullptr;
  const h2_pal_webrtc_api_t* api = nullptr;
  std::atomic<uint64_t> now_us{0};
  std::atomic<size_t> allocated{0}, freed{0};
  std::mutex lock;
  std::condition_variable changed;
  bool go = false, finish = false, snapshot_ready = false;
  bool active = false, available = false, read_frame = false;
  unsigned packets = 0, receive_calls = 0, send_attempts = 0, successes = 0;
  unsigned sent_packet = 0, channel_messages = 0, channel_bytes = 0,
           eos_reads = 0;
  uint64_t admitted_us = 0, sent_us = 0;
  const uint8_t* pending_data = nullptr;
  Snapshot snapshot{};
};
Fixture* current;
thread_local bool network_thread = false;
void* allocate(void* user, size_t len) {
  void* memory = std::malloc(len);
  if (memory) static_cast<Fixture*>(user)->allocated.fetch_add(1);
  return memory;
}
void release(void* user, void* memory) {
  if (memory) static_cast<Fixture*>(user)->freed.fetch_add(1);
  std::free(memory);
}
void run_network(void*) {
  network_thread = true;
  current->entry(current->context);
  network_thread = false;
}
int start(void* user, const h2_pal_task_options_t* options,
          h2_pal_task_entry_t entry, void* context, h2_pal_task_t** out) {
  if (!std::strcmp(options->name, "$h2peer/net")) {
    current->entry = entry;
    current->context = context;
    return current->real_task->vtable->start(user, options, run_network,
                                             nullptr, out);
  }
  return current->real_task->vtable->start(user, options, entry, context, out);
}
h2_pal_result_t clock_us(void* user, uint64_t* out) {
  *out = static_cast<Fixture*>(user)->now_us.load();
  return H2_PAL_OK;
}
h2_pal_result_t clock_ms(void* user, uint64_t* out) {
  *out = static_cast<Fixture*>(user)->now_us.load() / 1000u;
  return H2_PAL_OK;
}
int sleep_ms(void*, uint32_t ms) {
  auto& f = *current;
  if (network_thread) {
    std::unique_lock<std::mutex> lock(f.lock);
    if (!f.active && f.go) {
      /* The fake protocol has completed negotiation. Only the worker edits
       * these protocol-owned fields; no real connection is made. */
      f.peer->offer_started = 1;
      f.peer->remote_answer_set = 1;
      h2_atomic_int_store(&f.peer->state, H2_PAL_WEBRTC_PEER_CONNECTED,
                          H2_ATOMIC_SEQ_CST);
      h2_atomic_int_store(&f.channel->open, 1, H2_ATOMIC_SEQ_CST);
      f.now_us.store(0);
      f.active = true;
    }
    if (f.active && f.packets == kPackets && !f.snapshot_ready) {
      assert(ms == 1u);
      f.snapshot = {f.packets,
                    f.receive_calls,
                    f.send_attempts,
                    f.successes,
                    f.sent_packet,
                    f.channel_messages,
                    f.channel_bytes,
                    f.eos_reads,
                    f.admitted_us,
                    f.sent_us,
                    h2_atomic_ptr_load(&f.peer->rtp_pending,
                                       H2_ATOMIC_ACQUIRE) == nullptr};
      f.snapshot_ready = true;
      f.changed.notify_all();
      assert(f.changed.wait_for(lock, std::chrono::seconds(5),
                                [&] { return f.finish; }));
    }
  }
  f.now_us.fetch_add((uint64_t)ms * 1000u);
  return f.real_time->vtable->sleep_ms(f.real_time->user, ms);
}
h2_pal_result_t read_track(void*, uint8_t* out, size_t cap, size_t* len) {
  auto& f = *current;
  *len = 0;
  if (!f.active || !f.track_source || !f.available)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (!f.read_frame) {
    assert(cap >= 1);
    out[0] = kOpus;
    *len = 1;
    f.read_frame = true;
    return H2_PAL_OK;
  }
  /* Consuming EOS must wait until the preceding RTP was actually accepted. */
  assert(f.successes == 1);
  ++f.eos_reads;
  return H2_PAL_ERR_WOULD_BLOCK;
}
}  // namespace

extern "C" int h2_test_media_async_receive_supported(
    const h2_pal_webrtc_peer_t*) {
  return current->active;
}
extern "C" int h2_test_media_receive_datagram(h2_pal_webrtc_peer_t*,
                                              h2_pal_net_addr_t* addr,
                                              uint8_t* out, size_t cap,
                                              uint32_t) {
  auto& f = *current;
  ++f.receive_calls;
  if (f.packets == kPackets) return H2_PAL_ERR_WOULD_BLOCK;
  assert(cap >= 1);
  *addr = {};
  out[0] = 1;
  return 1;
}
extern "C" h2_pal_result_t h2_test_media_service_datagram(
    h2_pal_webrtc_peer_t* peer, h2_pal_net_addr_t*, uint8_t*, size_t len) {
  if (!len) return H2_PAL_OK;
  auto& f = *current;
  ++f.packets;
  f.now_us.fetch_add(kPacketCostUs / 2);
  if (f.packets == 1) {
    static const uint8_t bulk[4096] = {};
    for (unsigned i = 0; i < 4; ++i)
      assert(h2_pal_webrtc_channel_send(f.api, f.channel, bulk, sizeof(bulk),
                                        0) == H2_PAL_OK);
  }
  if (f.packets == kArrivalPacket) {
    f.admitted_us = f.now_us.load();
    if (f.track_source) {
      f.available = true;
    } else {
      assert(h2_pal_webrtc_peer_send_opus(f.api, peer, &kOpus, 1) == H2_PAL_OK);
    }
  }
  f.now_us.fetch_add(kPacketCostUs / 2);
  return H2_PAL_OK;
}
extern "C" h2_pal_result_t h2_test_media_send_opus(h2_pal_webrtc_peer_t* peer,
                                                   const uint8_t* data,
                                                   size_t len) {
  auto& f = *current;
  assert(len == 1 && data[0] == kOpus);
  assert(h2_atomic_ptr_load(&peer->rtp_pending, H2_ATOMIC_ACQUIRE) ==
         peer->rtp_storage);
  if (!f.pending_data) f.pending_data = data;
  assert(f.pending_data == data);
  ++f.send_attempts;
  if (f.send_attempts <= f.blocked_attempts) return H2_PAL_ERR_WOULD_BLOCK;
  ++f.successes;
  assert(f.successes == 1);
  f.sent_packet = f.packets;
  f.sent_us = f.now_us.load();
  return H2_PAL_OK;
}
extern "C" h2_pal_result_t h2_test_media_sctp_is_writable(h2_pal_webrtc_peer_t*,
                                                          bool* writable) {
  *writable = true;
  return H2_PAL_OK;
}
extern "C" h2_pal_result_t h2_test_media_channel_send(h2_pal_webrtc_channel_t*,
                                                      const uint8_t*,
                                                      size_t len, int) {
  assert(len == 4096);
  ++current->channel_messages;
  current->channel_bytes += (unsigned)len;
  return H2_PAL_OK;
}

static void run_case(bool track_source, unsigned blocked_attempts) {
  Fixture f{};
  f.track_source = track_source;
  f.blocked_attempts = blocked_attempts;
  current = &f;
  f.real_task = h2_desktop_platform_task_api();
  f.real_time = h2_desktop_platform_time_api();
  auto task_ops = *f.real_task->vtable;
  task_ops.start = start;
  const h2_pal_task_api_t task = {f.real_task->user, &task_ops};
  auto time_ops = *f.real_time->vtable;
  time_ops.get_monotonic_us = clock_us;
  time_ops.get_monotonic_ms = clock_ms;
  time_ops.sleep_ms = sleep_ms;
  const h2_pal_time_api_t time = {&f, &time_ops};
  h2_pal_mem_vtable_t mem_ops{};
  mem_ops.alloc = allocate;
  mem_ops.free = release;
  const h2_pal_mem_api_t memory = {&f, &mem_ops};
  h2_peer_config_t config{};
  config.mem = &memory;
  config.log = h2_desktop_platform_log_api();
  config.net = h2_pal_unsupported_net_api();
  config.queue = h2_desktop_platform_queue_api();
  config.sync = h2_desktop_platform_sync_api();
  config.task = &task;
  config.time = &time;
  config.crypto = h2_pal_unsupported_crypto_api();
  config.dtls = h2_pal_unsupported_dtls_api();
  config.sctp = h2_pal_unsupported_sctp_api();
  assert(h2_peer_create(&config, &f.owner) == H2_PAL_OK);
  f.api = h2_peer_webrtc_api(f.owner);
  assert(h2_pal_webrtc_peer_create(f.api, &f.peer) == H2_PAL_OK);
  h2_pal_webrtc_track_vtable_t track_ops{};
  track_ops.read = read_track;
  h2_pal_webrtc_track_t track{};
  track.vtable = &track_ops;
  assert(h2_pal_webrtc_peer_set_track(f.api, f.peer, &track) == H2_PAL_OK);
  h2_pal_webrtc_channel_config_t channel_config{};
  channel_config.label = {"bulk", 4};
  channel_config.ordered = 1;
  channel_config.reliable = 1;
  assert(h2_pal_webrtc_peer_create_data_channel(f.api, f.peer, &channel_config,
                                                &f.channel) == H2_PAL_OK);
  Snapshot snapshot{};
  {
    std::unique_lock<std::mutex> lock(f.lock);
    f.go = true;
    assert(f.changed.wait_for(lock, std::chrono::seconds(5),
                              [&] { return f.snapshot_ready; }));
    snapshot = f.snapshot;
    f.finish = true;
    f.changed.notify_all();
  }
  h2_pal_webrtc_peer_close(f.api, f.peer);
  h2_peer_destroy(&f.owner);
  assert(f.owner == nullptr && f.allocated.load() == f.freed.load());
  assert(snapshot.packets == kPackets && snapshot.receive_calls == kPackets);
  assert(snapshot.successes == 1 && snapshot.sent_packet < kPackets);
  assert(snapshot.sent_packet <= kArrivalPacket + blocked_attempts);
  assert(snapshot.sent_us - snapshot.admitted_us <=
         (blocked_attempts + 1u) * kPacketCostUs);
  assert(snapshot.send_attempts == blocked_attempts + 1u &&
         snapshot.pending_empty);
  assert(snapshot.channel_messages == 2 && snapshot.channel_bytes == 8192);
  if (track_source) assert(snapshot.eos_reads > 0);
  current = nullptr;
}
int main() {
  run_case(false, 0);
  run_case(false, 2);
  run_case(true, 0);
  run_case(true, 2);
}
