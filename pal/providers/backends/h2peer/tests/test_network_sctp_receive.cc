extern "C" {
#include "h2_peer_internal.h"
#include "providers/h2_peer_portable_backend.h"
#include "sctp.h"
#include "h2_sctp_internal.h"
#include "h2_sctp_test_peer.h"
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

/* Exercise the production worker, portable SCTP callback bridge, public
 * association APIs, event queue, poll and release. The transport seam replaces
 * ICE/DTLS and the SID-to-channel lookup; it does not implement SCTP delivery,
 * reassembly, accounting, event ownership or worker scheduling. */
namespace {
constexpr size_t kMessageBytes = 8192;
constexpr unsigned kMessages = 96;
using Packet = std::vector<uint8_t>;
struct Fixture {
  h2_sctp_test_endpoint_t remote{};
  h2_sctp_t* provider = nullptr;
  Sctp receiver{};
  DtlsSrtp dtls{};
  h2_peer_t* owner = nullptr;
  h2_pal_webrtc_peer_t* peer = nullptr;
  h2_pal_webrtc_channel_t* channel = nullptr;
  const h2_pal_webrtc_api_t* api = nullptr;
  const h2_pal_task_api_t* real_task = nullptr;
  const h2_pal_queue_api_t* real_queue = nullptr;
  h2_pal_task_entry_t entry = nullptr;
  void* context = nullptr;
  h2_pal_queue_t* events = nullptr;
  std::mutex lock;
  std::condition_variable changed;
  std::atomic<bool> go{false}, active{false}, block_consumer{true};
  std::atomic<uint64_t> now_ms{1};
  std::atomic<size_t> allocations{0}, frees{0};
  std::deque<Packet> replies;
  Packet last_data;
  std::set<uint32_t> dropped_tsns;
  uint32_t seed = 0x50390a;
  unsigned submitted = 0, delivered = 0, callback_wb = 0, callback_ok = 0;
  unsigned receive_calls = 0, service_calls = 0, empty_services = 0;
  unsigned sack_zero = 0, sack_open = 0, emit_wb = 0, attempts = 0;
  unsigned dropped_updates = 0, probes = 0, reordered = 0, duplicated = 0;
  unsigned sleeps = 0, held_sleeps = 0;
  size_t used = 0, max_used = 0, send_used = 0;
  unsigned event_count = 0;
  size_t event_bytes = 0;
  bool full = false, held = false, done = false;
  unsigned gap_sacks = 0;
};
Fixture* current;
thread_local bool network_thread = false;

uint32_t read32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}
bool is_data(const Packet& p) {
  return p.size() > 16 &&
         (p[12] == H2_SCTP_CHUNK_DATA || p[12] == H2_SCTP_CHUNK_I_DATA);
}
uint32_t random(Fixture& f) {
  f.seed ^= f.seed << 13;
  f.seed ^= f.seed >> 17;
  f.seed ^= f.seed << 5;
  return f.seed;
}
Packet payload(unsigned sequence) {
  Packet data(kMessageBytes);
  for (size_t i = 0; i < data.size(); ++i)
    data[i] = uint8_t(sequence * 31u + i * 17u + (i >> 8));
  return data;
}
void* allocate(void* user, size_t len) {
  void* p = std::malloc(len);
  if (p) static_cast<Fixture*>(user)->allocations.fetch_add(1);
  return p;
}
void release(void* user, void* p) {
  if (p) static_cast<Fixture*>(user)->frees.fetch_add(1);
  std::free(p);
}
int create_queue(void* user, const h2_pal_queue_config_t* config,
                 h2_pal_queue_t** out) {
  int rc = current->real_queue->vtable->create(user, config, out);
  if (rc == H2_PAL_OK && !std::strcmp(config->name, "h2peer/net/events"))
    current->events = *out;
  return rc;
}
int send_queue(void* user, h2_pal_queue_t* queue, const void* item,
               uint32_t timeout) {
  if (network_thread && queue == current->events &&
      current->block_consumer.load())
    return H2_PAL_ERR_WOULD_BLOCK;
  return current->real_queue->vtable->send(user, queue, item, timeout);
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
h2_pal_result_t clock_ms(void* user, uint64_t* out) {
  *out = static_cast<Fixture*>(user)->now_ms.load();
  return H2_PAL_OK;
}
h2_pal_result_t clock_us(void* user, uint64_t* out) {
  *out = static_cast<Fixture*>(user)->now_ms.load() * 1000u;
  return H2_PAL_OK;
}
void snapshot(Fixture& f) {
  f.used = f.receiver.association->receive_used;
  if (f.used > f.max_used) f.max_used = f.used;
  assert(f.used <= f.receiver.association->config.receive_buffer_size);
  size_t retained = 0;
  for (auto* fragment = f.receiver.association->rx_fragments; fragment;
       fragment = fragment->next)
    retained += fragment->data_len;
  assert(f.used == retained);
  f.send_used = f.remote.association->send_used;
  f.event_count =
      h2_atomic_uint_load(&f.peer->network_event_count, H2_ATOMIC_RELAXED);
  f.event_bytes =
      h2_atomic_size_load(&f.peer->network_event_bytes, H2_ATOMIC_RELAXED);
  if (f.used == f.receiver.association->config.receive_buffer_size &&
      f.remote.association->peer_receive_window == 0)
    f.full = true;
  if (!f.block_consumer.load() && f.event_bytes >= 16u * 1024u) f.held = true;
  f.done = f.submitted == kMessages && f.send_used == 0 && f.used == 0 &&
           f.event_count == 0 && f.delivered == kMessages;
  f.changed.notify_all();
}
int sleep_ms(void*, uint32_t ms) {
  auto& f = *current;
  if (network_thread && f.go.load()) {
    std::lock_guard<std::mutex> lock(f.lock);
    if (!f.active.load()) {
      f.peer->offer_started = 1;
      f.peer->remote_answer_set = 1;
      h2_atomic_int_store(&f.channel->open, 1, H2_ATOMIC_SEQ_CST);
      f.active.store(true);
    }
    ++f.sleeps;
    if (f.held && f.event_bytes >= 16u * 1024u) ++f.held_sleeps;
    snapshot(f);
    f.now_ms.fetch_add(ms);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return H2_PAL_OK;
}
h2_pal_result_t on_message(char* data, size_t len, void* user, uint16_t sid,
                           int is_text) {
  auto& f = *static_cast<Fixture*>(user);
  assert(sid == 0 && !is_text && len == kMessageBytes);
  h2_pal_result_t rc = h2_peer_webrtc_emit_channel_message(
      f.peer, f.channel, reinterpret_cast<uint8_t*>(data), len, is_text);
  if (rc == H2_PAL_ERR_WOULD_BLOCK)
    ++f.callback_wb;
  else {
    assert(rc == H2_PAL_OK);
    ++f.callback_ok;
  }
  return rc;
}
void deliver_replies(Fixture& f) {
  while (!f.replies.empty()) {
    auto p = std::move(f.replies.front());
    f.replies.pop_front();
    assert(h2_pal_sctp_association_input_packet(
               f.remote.api, f.remote.association, p.data(), p.size(),
               f.now_ms.load()) == H2_PAL_OK);
  }
}
void submit(Fixture& f) {
  while (f.submitted < kMessages) {
    auto data = payload(f.submitted);
    h2_pal_sctp_message_t msg{};
    msg.data = data.data();
    msg.len = data.size();
    msg.stream_id = 0;
    msg.ppid = PPID_BINARY;
    msg.reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE;
    auto rc = h2_pal_sctp_association_send_message(
        f.remote.api, f.remote.association, &msg, f.now_ms.load());
    if (rc == H2_PAL_ERR_WOULD_BLOCK) return;
    assert(rc == H2_PAL_OK);
    ++f.submitted;
  }
}
Packet take_remote(Fixture& f) {
  auto* p = f.remote.packet_head;
  if (!p) return {};
  f.remote.packet_head = p->next;
  if (!f.remote.packet_head) f.remote.packet_tail = nullptr;
  Packet data(p->data, p->data + p->len);
  std::free(p->data);
  std::free(p);
  return data;
}
void wait_for(Fixture& f, bool Fixture::* flag) {
  std::unique_lock<std::mutex> lock(f.lock);
  if (!f.changed.wait_for(lock, std::chrono::seconds(15),
                          [&] { return f.*flag; })) {
    std::fprintf(stderr,
                 "timeout submitted=%u delivered=%u used=%zu send=%zu "
                 "events=%u/%zu wb=%u sack=%u/%u probes=%u\n",
                 f.submitted, f.delivered, f.used, f.send_used, f.event_count,
                 f.event_bytes, f.callback_wb, f.sack_zero, f.sack_open,
                 f.probes);
    assert(false);
  }
}
}  // namespace

extern "C" h2_pal_result_t h2_test_sctp_emit(DtlsSrtp*, const uint8_t* data,
                                             size_t len) {
  auto& f = *current;
  if (f.active.load() && ++f.attempts % 11u == 0u) {
    ++f.emit_wb;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (len >= 28 && data[12] == H2_SCTP_CHUNK_SACK) {
    uint32_t window = read32(data + 20);
    if (data[24] || data[25]) ++f.gap_sacks;
    if (!window)
      ++f.sack_zero;
    else {
      ++f.sack_open;
      if (f.full && !f.dropped_updates) {
        ++f.dropped_updates;
        return H2_PAL_OK;  // Accepted locally, lost on the datagram network.
      }
    }
  }
  f.replies.emplace_back(data, data + len);
  return H2_PAL_OK;
}
extern "C" int h2_test_sctp_async_receive_supported(
    const h2_pal_webrtc_peer_t*) {
  return current->active.load();
}
extern "C" int h2_test_sctp_receive_datagram(h2_pal_webrtc_peer_t*,
                                             h2_pal_net_addr_t* addr,
                                             uint8_t* out, size_t cap,
                                             uint32_t timeout) {
  auto& f = *current;
  std::lock_guard<std::mutex> lock(f.lock);
  ++f.receive_calls;
  deliver_replies(f);
  submit(f);
  uint64_t deadline;
  assert(h2_pal_sctp_association_service(f.remote.api, f.remote.association,
                                         f.now_ms.load(),
                                         &deadline) == H2_PAL_OK);
  // Fixed-seed reordering only changes a packet queue, never SCTP state.
  if (f.remote.packet_head && f.remote.packet_head->next &&
      random(f) % 5u == 0u) {
    auto* first = f.remote.packet_head;
    auto* second = first->next;
    first->next = second->next;
    second->next = first;
    f.remote.packet_head = second;
    if (f.remote.packet_tail == second) f.remote.packet_tail = first;
    ++f.reordered;
  }
  Packet p;
  for (;;) {
    p = take_remote(f);
    if (p.empty()) break;
    if (is_data(p)) {
      const uint32_t tsn = read32(p.data() + 16);
      if (tsn % 17u == 0u && f.dropped_tsns.insert(tsn).second) continue;
      f.last_data = p;
    }
    break;
  }
  if (p.empty() && f.full && !f.block_consumer.load() &&
      f.remote.association->peer_receive_window == 0 && !f.last_data.empty()) {
    // Pion can probe/retransmit at zero credit. Model its duplicate DATA input
    // explicitly; the local sender has no independent zero-window probe timer.
    p = f.last_data;
    ++f.probes;
  }
  if (p.empty()) {
    f.now_ms.fetch_add(timeout);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  assert(p.size() <= cap);
  std::memset(addr, 0, sizeof(*addr));
  std::memcpy(out, p.data(), p.size());
  return static_cast<int>(p.size());
}
extern "C" h2_pal_result_t h2_test_sctp_service_datagram(h2_pal_webrtc_peer_t*,
                                                         h2_pal_net_addr_t*,
                                                         uint8_t* data,
                                                         size_t len) {
  auto& f = *current;
  std::lock_guard<std::mutex> lock(f.lock);
  ++f.service_calls;
  // This is the production PeerConnection ordering: service before DATA, and
  // service on an empty transport round too. ICE and DTLS are outside this
  // test.
  assert(sctp_service(&f.receiver) == 0);
  if (len) {
    sctp_incoming_data(&f.receiver, reinterpret_cast<char*>(data), len);
    if (is_data(Packet(data, data + len)) && f.service_calls % 29u == 0u) {
      sctp_incoming_data(&f.receiver, reinterpret_cast<char*>(data), len);
      ++f.duplicated;
    }
  } else
    ++f.empty_services;
  deliver_replies(f);
  snapshot(f);
  return H2_PAL_OK;
}

static void run_case(bool interleaved) {
  Fixture f;
  current = &f;
  f.real_task = h2_desktop_platform_task_api();
  f.real_queue = h2_desktop_platform_queue_api();
  auto task_ops = *f.real_task->vtable;
  task_ops.start = start;
  const h2_pal_task_api_t task = {f.real_task->user, &task_ops};
  auto queue_ops = *f.real_queue->vtable;
  queue_ops.create = create_queue;
  queue_ops.send = send_queue;
  const h2_pal_queue_api_t queue = {f.real_queue->user, &queue_ops};
  auto time_ops = *h2_desktop_platform_time_api()->vtable;
  time_ops.get_monotonic_ms = clock_ms;
  time_ops.get_monotonic_us = clock_us;
  time_ops.sleep_ms = sleep_ms;
  const h2_pal_time_api_t time = {&f, &time_ops};
  h2_pal_mem_vtable_t mem_ops{};
  mem_ops.alloc = allocate;
  mem_ops.free = release;
  const h2_pal_mem_api_t memory = {&f, &mem_ops};
  assert(h2_sctp_test_endpoint_init(&f.remote, H2_PAL_SCTP_ROLE_ACTIVE, 5000,
                                    5000, 1192, 65536, 262144,
                                    262144) == H2_PAL_OK);
  h2_sctp_config_t sctp_config{};
  sctp_config.mem = &memory;
  sctp_config.crypto = &f.remote.crypto_api;
  assert(h2_sctp_create(&sctp_config, &f.provider) == H2_PAL_OK);
  h2_peer_config_t config{};
  config.mem = &memory;
  config.log = h2_desktop_platform_log_api();
  config.net = h2_pal_unsupported_net_api();
  config.queue = &queue;
  config.sync = h2_desktop_platform_sync_api();
  config.task = &task;
  config.time = &time;
  config.crypto = h2_pal_unsupported_crypto_api();
  config.dtls = h2_pal_unsupported_dtls_api();
  config.sctp = h2_sctp_api(f.provider);
  assert(h2_peer_create(&config, &f.owner) == H2_PAL_OK);
  f.api = h2_peer_webrtc_api(f.owner);
  assert(h2_pal_webrtc_peer_create(f.api, &f.peer) == H2_PAL_OK);
  h2_pal_webrtc_channel_config_t channel_config{};
  channel_config.label = {"bulk", 4};
  channel_config.ordered = 1;
  channel_config.reliable = 1;
  channel_config.negotiated = 1;
  channel_config.has_stream_id = 1;
  channel_config.stream_id = 0;
  assert(h2_pal_webrtc_peer_create_data_channel(f.api, f.peer, &channel_config,
                                                &f.channel) == H2_PAL_OK);
  f.receiver.mem = &memory;
  f.receiver.log = config.log;
  f.receiver.api = config.sctp;
  f.receiver.time = &time;
  f.receiver.userdata = &f;
  sctp_onmessage(&f.receiver, on_message);
  assert(sctp_register_negotiated_channel(&f.receiver, "bulk", 0, 0, 0) == 0);
  f.dtls.role = DTLS_SRTP_ROLE_SERVER;
  assert(sctp_create_association(&f.receiver, &f.dtls) == 0);
  assert(h2_pal_sctp_association_start(f.remote.api, f.remote.association,
                                       f.now_ms.load()) == H2_PAL_OK);
  for (unsigned i = 0; i < 32 && !f.receiver.connected; ++i) {
    while (f.remote.packet_head) {
      auto p = take_remote(f);
      sctp_incoming_data(&f.receiver, reinterpret_cast<char*>(p.data()),
                         p.size());
    }
    deliver_replies(f);
  }
  assert(f.receiver.connected && f.remote.state == H2_PAL_SCTP_STATE_CONNECTED);
  f.remote.association->peer_interleaving = interleaved;
  f.receiver.association->peer_interleaving = interleaved;
  f.go.store(true);
  wait_for(f, &Fixture::full);
  f.block_consumer.store(false);
  wait_for(f, &Fixture::held);
  std::vector<h2_pal_webrtc_event_t> held;
  for (;;) {
    h2_pal_webrtc_event_t event{};
    auto rc = h2_pal_webrtc_peer_poll(f.api, f.peer, 0, &event);
    if (rc == H2_PAL_ERR_WOULD_BLOCK) break;
    assert(rc == H2_PAL_OK &&
           event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE);
    auto expected = payload(static_cast<unsigned>(held.size()));
    assert(event.data_len == expected.size() &&
           !std::memcmp(event.data, expected.data(), expected.size()));
    held.push_back(event);
  }
  assert(!held.empty());
  {
    std::unique_lock<std::mutex> lock(f.lock);
    unsigned received = f.receive_calls;
    unsigned slept = f.held_sleeps;
    assert(f.changed.wait_for(lock, std::chrono::seconds(5),
                              [&] { return f.held_sleeps >= slept + 3; }));
    assert(f.receive_calls == received);
    assert(f.event_count == held.size());  // poll alone does not return credit.
    assert(f.event_bytes == held.size() * kMessageBytes);
    f.delivered = static_cast<unsigned>(held.size());
  }
  for (auto& event : held) h2_pal_webrtc_event_release(&event);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  unsigned delivered = static_cast<unsigned>(held.size());
  while (delivered < kMessages && std::chrono::steady_clock::now() < deadline) {
    h2_pal_webrtc_event_t event{};
    auto rc = h2_pal_webrtc_peer_poll(f.api, f.peer, 0, &event);
    if (rc == H2_PAL_ERR_WOULD_BLOCK) {
      std::this_thread::yield();
      continue;
    }
    assert(rc == H2_PAL_OK &&
           event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE);
    auto expected = payload(delivered++);
    assert(event.data_len == expected.size() &&
           !std::memcmp(event.data, expected.data(), expected.size()));
    h2_pal_webrtc_event_release(&event);
    std::lock_guard<std::mutex> lock(f.lock);
    f.delivered = delivered;
  }
  wait_for(f, &Fixture::done);
  h2_pal_webrtc_peer_close(f.api, f.peer);
  h2_peer_destroy(&f.owner);
  assert(f.callback_wb > 0 && f.callback_ok == kMessages);
  assert(f.sack_zero > 0 && f.sack_open > 0 && f.gap_sacks > 0 &&
         f.dropped_updates == 1);
  assert(f.emit_wb > 0 && f.empty_services > 0 && f.probes > 0);
  assert(f.reordered > 0 && f.duplicated > 0 && !f.dropped_tsns.empty());
  assert(f.receiver.association->receive_used == 0);
  assert(f.receiver.association->rx_fragments == nullptr);
  assert(f.receiver.association->rx_assembly_begin == nullptr);
  assert(f.receiver.association->rx_assembly_len == 0);
  std::printf(
      "wire=%s seed=0x50390a bytes=%zu messages=%u max_receive=%zu "
      "callback_wb=%u sacks_zero/open=%u/%u emit_wb=%u "
      "lost_data=%zu reordered=%u duplicate=%u lost_update=%u "
      "probes=%u empty_service=%u final_receive=%zu final_events=%u/%zu\n",
      interleaved ? "I-DATA" : "DATA", kMessages * kMessageBytes, f.delivered,
      f.max_used, f.callback_wb, f.sack_zero, f.sack_open, f.emit_wb,
      f.dropped_tsns.size(), f.reordered, f.duplicated, f.dropped_updates,
      f.probes, f.empty_services, f.used, f.event_count, f.event_bytes);
  f.active.store(false);
  sctp_destroy_association(&f.receiver);
  assert(h2_sctp_destroy(&f.provider) == H2_PAL_OK);
  h2_sctp_test_endpoint_deinit(&f.remote);
  assert(f.allocations.load() == f.frees.load());
  current = nullptr;
}
int main() {
  run_case(false);
  run_case(true);
}
