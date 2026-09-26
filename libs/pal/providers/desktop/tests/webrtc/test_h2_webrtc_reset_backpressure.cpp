#include "h2_desktop_app_support.h"
#include "h2_peer.h"
#include "h2_sctp.h"
extern "C" {
#include "h2_webrtc_pion_fixture.h"
}

#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

// All interception runs on H2Peer's real protocol owner. Only one transport
// emission is refused; SCTP packets, callbacks and resets remain production
// code.
struct ResetLink {
  const h2_pal_sctp_api_t *real;
  h2_pal_sctp_callbacks_t callbacks{};
  h2_pal_sctp_vtable_t vtable{};
  h2_pal_sctp_api_t api{this, &vtable};
  bool injected = false;
  bool pressure = true;
  bool block_emit = false;
  std::atomic<unsigned> blocked{0};
  std::atomic<unsigned> completed{0};
  std::atomic<unsigned> incoming{0};

  explicit ResetLink(const h2_pal_sctp_api_t *provider) : real(provider) {
    vtable.association_create = [](void *u,
                                   const h2_pal_sctp_association_config_t *c,
                                   h2_pal_sctp_association_t **a) {
      auto &link = *static_cast<ResetLink *>(u);
      link.callbacks = c->callbacks;
      auto config = *c;
      config.callbacks = {&link, emit, state, message, reset_event};
      return h2_pal_sctp_association_create(link.real, &config, a);
    };
    vtable.association_start = [](void *u, h2_pal_sctp_association_t *a,
                                  uint64_t t) {
      return h2_pal_sctp_association_start(static_cast<ResetLink *>(u)->real, a,
                                           t);
    };
    vtable.association_input_packet = [](void *u, h2_pal_sctp_association_t *a,
                                         const uint8_t *p, size_t n,
                                         uint64_t t) {
      return h2_pal_sctp_association_input_packet(
          static_cast<ResetLink *>(u)->real, a, p, n, t);
    };
    vtable.association_service = [](void *u, h2_pal_sctp_association_t *a,
                                    uint64_t t, uint64_t *d) {
      return h2_pal_sctp_association_service(static_cast<ResetLink *>(u)->real,
                                             a, t, d);
    };
    vtable.association_send_message = [](void *u, h2_pal_sctp_association_t *a,
                                         const h2_pal_sctp_message_t *m,
                                         uint64_t t) {
      return h2_pal_sctp_association_send_message(
          static_cast<ResetLink *>(u)->real, a, m, t);
    };
    vtable.association_is_writable = [](void *u, h2_pal_sctp_association_t *a,
                                        bool *w) {
      return h2_pal_sctp_association_is_writable(
          static_cast<ResetLink *>(u)->real, a, w);
    };
    vtable.association_reset_stream = [](void *u, h2_pal_sctp_association_t *a,
                                         uint16_t sid, uint64_t t) {
      auto &link = *static_cast<ResetLink *>(u);
      if (link.pressure && !link.injected) {
        link.injected = true;
        link.block_emit = true;
        // Concurrent telemetry on the long-lived channel leaves a retained
        // DATA packet immediately before the first request-channel reset.
        static const uint8_t payload[] = "telemetry-before-reset";
        h2_pal_sctp_message_t telemetry{};
        telemetry.data = payload;
        telemetry.len = sizeof(payload) - 1u;
        telemetry.stream_id = 1u;
        telemetry.ppid = 51u;
        telemetry.reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE;
        auto result =
            h2_pal_sctp_association_send_message(link.real, a, &telemetry, t);
        if (result != H2_PAL_OK)
          return result;
      }
      return h2_pal_sctp_association_reset_stream(link.real, a, sid, t);
    };
    vtable.association_shutdown = [](void *u, h2_pal_sctp_association_t *a,
                                     uint64_t t) {
      return h2_pal_sctp_association_shutdown(static_cast<ResetLink *>(u)->real,
                                              a, t);
    };
    vtable.association_abort = [](void *u, h2_pal_sctp_association_t *a,
                                  h2_pal_result_t r, uint64_t t) {
      return h2_pal_sctp_association_abort(static_cast<ResetLink *>(u)->real, a,
                                           r, t);
    };
    vtable.association_close = [](void *u, h2_pal_sctp_association_t **a) {
      return h2_pal_sctp_association_close(static_cast<ResetLink *>(u)->real,
                                           a);
    };
  }
  static h2_pal_result_t emit(void *u, h2_pal_sctp_association_t *a,
                              const uint8_t *p, size_t n) {
    auto &link = *static_cast<ResetLink *>(u);
    if (link.block_emit) {
      link.block_emit = false;
      ++link.blocked;
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    return link.callbacks.emit_packet(link.callbacks.user, a, p, n);
  }
  static void state(void *u, h2_pal_sctp_association_t *a,
                    h2_pal_sctp_state_t s, h2_pal_result_t r) {
    auto &link = *static_cast<ResetLink *>(u);
    link.callbacks.on_state(link.callbacks.user, a, s, r);
  }
  static h2_pal_result_t message(void *u, h2_pal_sctp_association_t *a,
                                 const h2_pal_sctp_received_message_t *m) {
    auto &link = *static_cast<ResetLink *>(u);
    return link.callbacks.on_message(link.callbacks.user, a, m);
  }
  static void reset_event(void *u, h2_pal_sctp_association_t *a,
                          const h2_pal_sctp_stream_reset_event_t *e) {
    auto &link = *static_cast<ResetLink *>(u);
    if (e->result == H2_PAL_OK) {
      if (e->direction == H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED)
        ++link.completed;
      else
        ++link.incoming;
    }
    link.callbacks.on_stream_reset(link.callbacks.user, a, e);
  }
};

struct Fixture {
  h2_webrtc_pion_fixture_t value{};
  ~Fixture() { h2_webrtc_pion_fixture_stop(&value); }
};

struct Events {
  const h2_pal_webrtc_api_t *api;
  h2_pal_webrtc_peer_t *peer;
  std::string offer;
  unsigned opened = 0;
  unsigned closed = 0;
  unsigned echoed = 0;
  unsigned telemetry = 0;
  bool seen[300]{};
  unsigned reused = 0;

  void poll() {
    h2_pal_webrtc_event_t event{};
    auto result = h2_pal_webrtc_peer_poll(api, peer, 10, &event);
    if (result == H2_PAL_ERR_TIMEOUT || result == H2_PAL_ERR_WOULD_BLOCK)
      return;
    require(result == H2_PAL_OK, "peer poll failed");
    bool failed = false;
    if (event.kind == H2_PAL_WEBRTC_EVENT_LOCAL_SDP)
      offer.assign(event.sdp.data, event.sdp.len);
    if (event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_STATE) {
      if (event.channel_state == H2_PAL_WEBRTC_CHANNEL_OPEN) {
        ++opened;
        const unsigned sid = event.channel_info.stream_id;
        if (sid < 300u) {
          reused += seen[sid] ? 1u : 0u;
          seen[sid] = true;
        }
      }
      if (event.channel_state == H2_PAL_WEBRTC_CHANNEL_CLOSED)
        ++closed;
      failed = event.channel_state == H2_PAL_WEBRTC_CHANNEL_ERROR;
    }
    if (event.kind == H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE) {
      const std::string payload(reinterpret_cast<const char *>(event.data),
                                event.data_len);
      if (payload == "server-echo-text:telemetry-before-reset")
        ++telemetry;
      else if (payload == "server-service-ack-binary:request")
        ++echoed;
      else
        failed = true;
    }
    failed |= event.kind == H2_PAL_WEBRTC_EVENT_ERROR;
    h2_pal_webrtc_event_release(&event);
    require(!failed, "unexpected peer event or payload");
  }
  template <typename Predicate> void wait(Predicate done) {
    uint64_t now = 0;
    const auto *time = h2_desktop_platform_time_api();
    require(h2_pal_time_get_monotonic_ms(time, &now) == H2_PAL_OK,
            "clock failed");
    const auto end = now + 10000u;
    while (!done() && now < end) {
      poll();
      require(h2_pal_time_get_monotonic_ms(time, &now) == H2_PAL_OK,
              "clock failed");
    }
    require(done(), "event deadline exceeded");
  }
};

static void run(const char *server, bool pressure) {
  Fixture fixture;
  require(h2_webrtc_pion_fixture_start(&fixture.value, server, "udp") == 0,
          "fixture start failed");
  h2::desktop::OwnedNetworkServices services;
  require(h2::desktop::open_network_services(false, true, &services) ==
              H2_PAL_OK,
          "providers failed");
  auto *original = static_cast<h2_peer_t *>(services.peer_handle);
  h2_peer_destroy(&original);
  services.peer_handle = nullptr;
  ResetLink link(h2_sctp_api(static_cast<h2_sctp_t *>(services.sctp_handle)));
  link.pressure = pressure;
  const h2_peer_config_t config = {h2_desktop_platform_default_allocator(),
                                   h2_desktop_platform_log_api(),
                                   h2::desktop::host_net_api(),
                                   h2_desktop_platform_queue_api(),
                                   h2_desktop_platform_sync_api(),
                                   h2_desktop_platform_task_api(),
                                   h2_desktop_platform_time_api(),
                                   services.crypto(),
                                   services.dtls(),
                                   &link.api};
  h2_peer_t *owner = nullptr;
  require(h2_peer_create(&config, &owner) == H2_PAL_OK, "H2Peer create failed");
  services.peer_handle = owner;
  // Keep the decorator alive until the owner task and all callbacks stop, even
  // on an assertion failure that unwinds the scenario.
  struct CloseOwner {
    h2::desktop::OwnedNetworkServices &services;
    ~CloseOwner() { services.reset(); }
  } close_owner{services};
  const auto *api = services.webrtc();
  h2_pal_webrtc_peer_t *peer = nullptr;
  require(h2_pal_webrtc_peer_create(api, &peer) == H2_PAL_OK,
          "peer create failed");
  Events events{api, peer, {}};
  auto create_channel = [&](const char *label) {
    h2_pal_webrtc_channel_config_t channel_config{};
    channel_config.label = {label, strlen(label)};
    channel_config.ordered = 1;
    channel_config.reliable = 1;
    h2_pal_webrtc_channel_t *channel = nullptr;
    auto result = h2_pal_webrtc_peer_create_data_channel(
        api, peer, &channel_config, &channel);
    if (result != H2_PAL_OK)
      fprintf(stderr,
              "reset pressure: opened=%u closed=%u completed=%u incoming=%u "
              "create=%d\n",
              events.opened, events.closed, link.completed.load(),
              link.incoming.load(), result);
    require(result == H2_PAL_OK, "SID allocation failed");
    return channel;
  };
  (void)create_channel("packet");
  (void)create_channel("event");
  require(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK,
          "offer failed");
  events.wait([&] { return !events.offer.empty(); });
  char answer[16384];
  size_t answer_len = 0;
  require(h2_webrtc_pion_fixture_exchange_performance(
              &fixture.value, {events.offer.data(), events.offer.size()},
              answer, sizeof(answer), &answer_len) == 0,
          "SDP exchange failed");
  require(h2_pal_webrtc_peer_set_remote_sdp(api, peer, H2_PAL_WEBRTC_SDP_ANSWER,
                                            {answer, answer_len}) == H2_PAL_OK,
          "answer failed");
  events.wait([&] { return events.opened == 2; });
  constexpr unsigned cycles = 512;
  for (unsigned cycle = 0; cycle < cycles; ++cycle) {
    auto *channel = create_channel("giznet/v1/service/0");
    events.wait([&] { return events.opened == cycle + 3; });
    const uint8_t payload[] = "request";
    require(h2_pal_webrtc_channel_send(api, channel, payload,
                                       sizeof(payload) - 1u, 0) == H2_PAL_OK,
            "request send failed");
    // The fixture echoes and closes its side. Keep issuing real requests after
    // each local CLOSED event, as a service consumer does; no Peer recreation.
    events.wait([&] {
      return events.closed == cycle + 1 && events.echoed == cycle + 1;
    });
  }
  events.wait([&] {
    return link.completed == cycles && events.telemetry == (pressure ? 1u : 0u);
  });
  require(link.blocked == (pressure ? 1u : 0u) && link.incoming == cycles &&
              events.reused > 0,
          "incomplete reset evidence");
  h2_webrtc_channel_stats_t stats{};
  require(h2_webrtc_pion_fixture_channel_stats(&fixture.value, &stats) == 0 &&
              stats.current == 2 && stats.created == cycles + 2 &&
              stats.closed == cycles,
          "remote channels did not drain");
  fprintf(stderr,
          "reset pressure: cycles=%u completed=%u incoming=%u reused=%u "
          "blocked=%u live=%llu\n",
          cycles, link.completed.load(), link.incoming.load(), events.reused,
          link.blocked.load(), stats.current);
}

int main(int argc, char **argv) {
  if (argc != 2 && !(argc == 3 && strcmp(argv[2], "--no-backpressure") == 0))
    return 2;
  try {
    run(argv[1], argc == 2);
    return 0;
  } catch (const std::exception &error) {
    fprintf(stderr, "reset pressure: %s\n", error.what());
    return 1;
  }
}
