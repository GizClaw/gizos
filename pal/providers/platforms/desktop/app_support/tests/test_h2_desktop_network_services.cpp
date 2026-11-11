#include "h2_desktop_app_support.h"
#include "h2_peer.h"
#include "h2_sctp.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <condition_variable>
#include <mutex>
#include <thread>

static h2_pal_mqtt_client_config_t client_config(uint8_t* buffer,
                                                 size_t buffer_len) {
  h2_pal_mqtt_client_config_t config;
  memset(&config, 0, sizeof(config));
  config.endpoint.host.data = "localhost";
  config.endpoint.host.len = strlen("localhost");
  config.endpoint.port = 1883u;
  config.client_id.data = "desktop-adapter-test";
  config.client_id.len = strlen("desktop-adapter-test");
  config.transport = H2_PAL_MQTT_TRANSPORT_TCP;
  config.keepalive_sec = 30u;
  config.connect_timeout_ms = 100u;
  config.operation_timeout_ms = 50u;
  config.clean_session = 1;
  config.network_buffer = buffer;
  config.network_buffer_len = buffer_len;
  return config;
}

static bool fail_join;
static int injected_join(void* user, h2_pal_task_t* task) {
  if (fail_join) return H2_PAL_ERR_IO;
  const auto* real = h2_desktop_platform_task_api();
  return real->vtable->join(user, task);
}

static void test_retained_peer_keeps_dependencies(void) {
  h2::desktop::OwnedNetworkServices services;
  assert(h2::desktop::open_network_services(false, true, &services) ==
         H2_PAL_OK);
  auto* initial = static_cast<h2_peer_t*>(services.peer_handle);
  h2_peer_destroy(&initial);
  assert(initial == nullptr);
  services.peer_handle = nullptr;
  const auto* real_task = h2_desktop_platform_task_api();
  auto task_vtable = *real_task->vtable;
  task_vtable.join = injected_join;
  const h2_pal_task_api_t task = {real_task->user, &task_vtable};
  h2_peer_config_t config = {};
  config.mem = h2_desktop_platform_default_allocator();
  config.log = h2_desktop_platform_log_api();
  config.net = h2::desktop::host_net_api();
  config.queue = h2_desktop_platform_queue_api();
  config.sync = h2_desktop_platform_sync_api();
  config.task = &task;
  config.time = h2_desktop_platform_time_api();
  config.crypto = services.crypto();
  config.dtls = services.dtls();
  config.sctp = h2_sctp_api(static_cast<h2_sctp_t*>(services.sctp_handle));
  h2_peer_t* owner = nullptr;
  assert(h2_peer_create(&config, &owner) == H2_PAL_OK);
  services.peer_handle = owner;
  h2_pal_webrtc_peer_t* peer = nullptr;
  assert(h2_pal_webrtc_peer_create(services.webrtc(), &peer) == H2_PAL_OK);
  const auto* sctp = services.sctp_handle;
  const auto* crypto = services.crypto();
  fail_join = true;
  assert(services.reset() == H2_PAL_ERR_IO);
  assert(services.peer_handle == owner && services.sctp_handle == sctp);
  assert(services.wolfssl_owner && services.crypto() == crypto);
  fail_join = false;
  assert(services.reset() == H2_PAL_OK);
  assert(services.peer_handle == nullptr && services.sctp_handle == nullptr &&
         !services.wolfssl_owner);
  assert(services.reset() == H2_PAL_OK);
}

static void test_concurrent_sctp_associations(void) {
  h2::desktop::OwnedNetworkServices services;
  assert(h2::desktop::open_network_services(false, true, &services) ==
         H2_PAL_OK);
  const auto* api = h2_sctp_api(static_cast<h2_sctp_t*>(services.sctp_handle));
  h2_pal_sctp_association_config_t config = {};
  config.role = H2_PAL_SCTP_ROLE_PASSIVE;
  config.local_port = config.remote_port = 5000u;
  config.inbound_streams = config.outbound_streams = 2u;
  config.max_packet_size = 256u;
  config.max_message_size = 128u;
  config.send_buffer_size = config.receive_buffer_size = 4096u;
  config.cookie_lifetime_ms = 60000u;
  config.callbacks.emit_packet = [](void*, h2_pal_sctp_association_t*,
                                    const uint8_t*, size_t) -> h2_pal_result_t {
    return H2_PAL_OK;
  };
  config.callbacks.on_state = [](void*, h2_pal_sctp_association_t*,
                                 h2_pal_sctp_state_t, h2_pal_result_t) {};
  config.callbacks.on_message =
      [](void*, h2_pal_sctp_association_t*,
         const h2_pal_sctp_received_message_t*) -> h2_pal_result_t {
    return H2_PAL_OK;
  };
  config.callbacks.on_stream_reset =
      [](void*, h2_pal_sctp_association_t*,
         const h2_pal_sctp_stream_reset_event_t*) {};

  constexpr unsigned owners = 8u;
  std::mutex mutex;
  std::condition_variable changed;
  unsigned arrived = 0u, generation = 0u;
  const auto barrier = [&] {
    std::unique_lock<std::mutex> lock(mutex);
    const unsigned current = generation;
    if (++arrived == owners) {
      arrived = 0u;
      ++generation;
      changed.notify_all();
    } else {
      changed.wait(lock, [&] { return generation != current; });
    }
  };
  // Independent protocol owners share the Desktop SCTP provider. Align both
  // lifecycle boundaries so registry insertions and removals overlap.
  std::thread workers[owners];
  for (auto& worker : workers) {
    worker = std::thread([&] {
      for (unsigned round = 0u; round < 1024u; ++round) {
        h2_pal_sctp_association_t* association = nullptr;
        barrier();
        assert(h2_pal_sctp_association_create(api, &config, &association) ==
               H2_PAL_OK);
        barrier();
        assert(h2_pal_sctp_association_close(api, &association) == H2_PAL_OK);
        assert(association == nullptr);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  assert(services.reset() == H2_PAL_OK);
}

int main(void) {
  test_concurrent_sctp_associations();
  test_retained_peer_keeps_dependencies();
  h2::desktop::OwnedNetworkServices services;
  assert(h2::desktop::open_network_services(true, false, &services) ==
         H2_PAL_OK);
  const h2_pal_mqtt_api_t* api = services.mqtt();
  assert(api != NULL);
  assert(api == services.mqtt());
  assert(api->user != NULL);
  assert(api->vtable != NULL);
  assert(api->vtable->open != NULL);
  assert(api->vtable->connect != NULL);
  assert(api->vtable->disconnect != NULL);
  assert(api->vtable->publish != NULL);
  assert(api->vtable->subscribe != NULL);
  assert(api->vtable->unsubscribe != NULL);
  assert(api->vtable->process != NULL);
  assert(api->vtable->close != NULL);

  h2_pal_mqtt_client_t* client = NULL;
  h2_pal_mqtt_client_config_t invalid_config;
  memset(&invalid_config, 0, sizeof(invalid_config));
  assert(h2_pal_mqtt_open(api, &invalid_config, &client) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(client == NULL);

  uint8_t buffer[512];
  h2_pal_mqtt_client_config_t config = client_config(buffer, sizeof(buffer));
  assert(h2_pal_mqtt_open(api, &config, &client) == H2_PAL_OK);
  assert(client != NULL);
  h2_pal_mqtt_close(api, client);
  return 0;
}
