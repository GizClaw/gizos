#include "ice_transport.h"

#include <assert.h>
#include <string.h>

typedef struct test_tcp_state {
  h2_pal_net_addr_t bound_source;
  h2_pal_net_addr_t connected_remote;
  uint8_t sent[CONFIG_MTU + 2u];
  size_t sent_len;
  size_t send_chunk;
  const uint8_t* receive;
  size_t receive_len;
  size_t receive_offset;
  size_t receive_chunk;
  int connect_result;
  int connect_result_after;
  int connect_calls;
  int send_error;
  int close_count;
} test_tcp_state_t;

static int test_tcp_open_bound(
    void* user, h2_pal_net_family_t family,
    const h2_pal_net_bind_t* bind_config,
    h2_pal_net_socket_t* out_socket) {
  test_tcp_state_t* state = user;
  assert(bind_config != NULL &&
         bind_config->type == H2_PAL_NET_BIND_SOURCE_ADDR);
  assert(bind_config->source_addr.family == family);
  state->bound_source = bind_config->source_addr;
  *out_socket = 7;
  return H2_PAL_OK;
}

static h2_pal_result_t test_tcp_connect(
    void* user, h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t* remote_addr, uint32_t timeout_ms) {
  test_tcp_state_t* state = user;
  assert(socket_fd == 7);
  (void)timeout_ms;
  state->connected_remote = *remote_addr;
  int result = state->connect_calls == 0
                   ? state->connect_result
                   : state->connect_result_after;
  state->connect_calls++;
  return result;
}

static int test_tcp_send_timeout(
    void* user, h2_pal_net_socket_t socket_fd,
    const uint8_t* data, size_t len, uint32_t timeout_ms) {
  test_tcp_state_t* state = user;
  assert(socket_fd == 7);
  (void)timeout_ms;
  if (state->send_error != 0) {
    return state->send_error;
  }
  if (state->send_chunk == 0u) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  size_t chunk = len < state->send_chunk ? len : state->send_chunk;
  assert(state->sent_len + chunk <= sizeof(state->sent));
  memcpy(state->sent + state->sent_len, data, chunk);
  state->sent_len += chunk;
  return (int)chunk;
}

static int test_tcp_recv(
    void* user, h2_pal_net_socket_t socket_fd,
    uint8_t* data, size_t len, uint32_t timeout_ms) {
  test_tcp_state_t* state = user;
  assert(socket_fd == 7);
  (void)timeout_ms;
  if (state->receive_offset == state->receive_len) {
    return H2_PAL_ERR_CLOSED;
  }
  size_t available = state->receive_len - state->receive_offset;
  size_t chunk = len < available ? len : available;
  if (state->receive_chunk != 0u && chunk > state->receive_chunk) {
    chunk = state->receive_chunk;
  }
  memcpy(data, state->receive + state->receive_offset, chunk);
  state->receive_offset += chunk;
  return (int)chunk;
}

static void test_tcp_close(void* user, h2_pal_net_socket_t socket_fd) {
  test_tcp_state_t* state = user;
  assert(socket_fd == 7);
  state->close_count++;
}

static const h2_pal_net_vtable_t test_net_vtable = {
    .tcp_open_bound = test_tcp_open_bound,
    .tcp_connect = test_tcp_connect,
    .tcp_send_timeout = test_tcp_send_timeout,
    .tcp_recv = test_tcp_recv,
    .close = test_tcp_close,
};

static void test_addresses(
    h2_pal_net_addr_t* source, h2_pal_net_addr_t* remote) {
  memset(source, 0, sizeof(*source));
  memset(remote, 0, sizeof(*remote));
  source->family = H2_PAL_NET_FAMILY_IPV4;
  source->port = 9u;
  source->ip[0] = 127u;
  source->ip[3] = 1u;
  *remote = *source;
  remote->port = 5000u;
}

static void test_partial_send_and_backpressure(void) {
  test_tcp_state_t state = {.send_chunk = 1u};
  h2_pal_net_api_t net = {.user = &state, .vtable = &test_net_vtable};
  h2_pal_net_addr_t source;
  h2_pal_net_addr_t remote;
  test_addresses(&source, &remote);
  IceTransport transport;
  assert(ice_transport_init_tcp(
             &transport, &net, &source, &remote, 25u) == H2_PAL_OK);
  assert(state.bound_source.port == 0u);
  assert(state.connected_remote.port == remote.port);
  static const uint8_t packet[] = {'a', 'b', 'c'};
  assert(ice_transport_send_packet(
             &transport, packet, sizeof(packet), 0u) ==
         (int)sizeof(packet));
  assert(ice_transport_send_packet(
             &transport, packet, sizeof(packet), 0u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  while (ice_transport_flush(&transport, 0u) == H2_PAL_ERR_WOULD_BLOCK) {
  }
  static const uint8_t expected[] = {0u, 3u, 'a', 'b', 'c'};
  assert(state.sent_len == sizeof(expected));
  assert(memcmp(state.sent, expected, sizeof(expected)) == 0);
  ice_transport_close(&transport);
  ice_transport_close(&transport);
  assert(state.close_count == 1);
}

static void test_fragmented_and_coalesced_receive(void) {
  static const uint8_t stream[] = {
      0u, 0u,
      0u, 3u, 'a', 'b', 'c',
      0u, 2u, 'd', 'e',
  };
  test_tcp_state_t state = {
      .send_chunk = 8u,
      .receive = stream,
      .receive_len = sizeof(stream),
      .receive_chunk = 1u,
  };
  h2_pal_net_api_t net = {.user = &state, .vtable = &test_net_vtable};
  h2_pal_net_addr_t source;
  h2_pal_net_addr_t remote;
  test_addresses(&source, &remote);
  IceTransport transport;
  assert(ice_transport_init_tcp(
             &transport, &net, &source, &remote, 0u) == H2_PAL_OK);
  uint8_t packet[CONFIG_MTU];
  int received = 0;
  while ((received = ice_transport_receive_packet(
              &transport, NULL, packet, sizeof(packet), 0u)) == 0) {
  }
  assert(received == 3 && memcmp(packet, "abc", 3u) == 0);
  while ((received = ice_transport_receive_packet(
              &transport, NULL, packet, sizeof(packet), 0u)) == 0) {
  }
  assert(received == 2 && memcmp(packet, "de", 2u) == 0);
  assert(ice_transport_receive_packet(
             &transport, NULL, packet, sizeof(packet), 0u) ==
         H2_PAL_ERR_CLOSED);
  ice_transport_close(&transport);
  assert(state.close_count == 1);
}

static void test_timeout_preserves_pending_frame(void) {
  test_tcp_state_t state = {
      .send_error = H2_PAL_ERR_TIMEOUT,
  };
  h2_pal_net_api_t net = {.user = &state, .vtable = &test_net_vtable};
  h2_pal_net_addr_t source;
  h2_pal_net_addr_t remote;
  test_addresses(&source, &remote);
  IceTransport transport;
  assert(ice_transport_init_tcp(
             &transport, &net, &source, &remote, 0u) == H2_PAL_OK);
  static const uint8_t packet[] = {'x', 'y'};
  assert(ice_transport_send_packet(
             &transport, packet, sizeof(packet), 10u) ==
         (int)sizeof(packet));
  assert(state.sent_len == 0u);
  assert(ice_transport_send_packet(
             &transport, packet, sizeof(packet), 0u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  state.send_error = 0;
  state.send_chunk = sizeof(state.sent);
  assert(ice_transport_flush(&transport, 0u) == H2_PAL_OK);
  static const uint8_t expected[] = {0u, 2u, 'x', 'y'};
  assert(state.sent_len == sizeof(expected));
  assert(memcmp(state.sent, expected, sizeof(expected)) == 0);
  ice_transport_close(&transport);
  assert(state.close_count == 1);
}

static void test_oversize_and_connect_lifecycle(void) {
  static const uint8_t oversize[] = {
      (uint8_t)((CONFIG_MTU + 1u) >> 8u),
      (uint8_t)((CONFIG_MTU + 1u) & 0xffu),
  };
  test_tcp_state_t state = {
      .send_chunk = 8u,
      .receive = oversize,
      .receive_len = sizeof(oversize),
  };
  h2_pal_net_api_t net = {.user = &state, .vtable = &test_net_vtable};
  h2_pal_net_addr_t source;
  h2_pal_net_addr_t remote;
  test_addresses(&source, &remote);
  IceTransport transport;
  assert(ice_transport_init_tcp(
             &transport, &net, &source, &remote, 0u) == H2_PAL_OK);
  uint8_t packet[CONFIG_MTU];
  assert(ice_transport_receive_packet(
             &transport, NULL, packet, sizeof(packet), 0u) ==
         H2_PAL_ERR_NO_SPACE);
  ice_transport_close(&transport);
  assert(state.close_count == 1);

  memset(&state, 0, sizeof(state));
  state.connect_result = H2_PAL_ERR_TIMEOUT;
  state.connect_result_after = H2_PAL_OK;
  assert(ice_transport_init_tcp(
             &transport, &net, &source, &remote, 10u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(state.close_count == 0);
  assert(transport.tcp_socket.fd == 7 && !transport.tcp_connected);
  assert(ice_transport_send_packet(
             &transport, packet, 1u, 0u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(ice_transport_progress_tcp_connect(&transport, 10u) == H2_PAL_OK);
  assert(state.connect_calls == 2 && transport.tcp_connected);
  ice_transport_close(&transport);
  assert(state.close_count == 1);

  memset(&state, 0, sizeof(state));
  state.connect_result = H2_PAL_ERR_IO;
  assert(ice_transport_init_tcp(
             &transport, &net, &source, &remote, 10u) == H2_PAL_ERR_IO);
  assert(state.close_count == 1);
  ice_transport_close(&transport);
  assert(state.close_count == 1);
}

int main(void) {
  test_partial_send_and_backpressure();
  test_fragmented_and_coalesced_receive();
  test_timeout_preserves_pending_frame();
  test_oversize_and_connect_lifecycle();
  return 0;
}
