#include "ice_transport.h"

#include <string.h>

static void ice_transport_reset_receive(IceTransport* transport) {
  transport->receive_prefix_len = 0u;
  transport->receive_len = 0u;
  transport->receive_expected = 0u;
}

void ice_transport_init_udp(
    IceTransport* transport, UdpSocket* socket,
    const h2_pal_net_addr_t* remote_addr) {
  memset(transport, 0, sizeof(*transport));
  transport->protocol = ICE_TRANSPORT_UDP;
  transport->udp_socket = socket;
  transport->tcp_socket.fd = -1;
  transport->remote_addr = *remote_addr;
}

int ice_transport_init_tcp(
    IceTransport* transport, const h2_pal_net_api_t* net,
    const h2_pal_net_addr_t* source_addr,
    const h2_pal_net_addr_t* remote_addr, uint32_t timeout_ms) {
  if (transport == NULL || net == NULL || source_addr == NULL ||
      remote_addr == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(transport, 0, sizeof(*transport));
  transport->protocol = ICE_TRANSPORT_TCP;
  transport->tcp_socket.fd = -1;
  transport->source_addr = *source_addr;
  transport->remote_addr = *remote_addr;
  int result = tcp_socket_connect(
      &transport->tcp_socket, net, source_addr, remote_addr, timeout_ms);
  transport->tcp_connected = result == H2_PAL_OK;
  return result;
}

int ice_transport_progress_tcp_connect(
    IceTransport* transport, uint32_t timeout_ms) {
  if (transport == NULL || transport->protocol != ICE_TRANSPORT_TCP ||
      transport->tcp_socket.fd < 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (transport->tcp_connected) {
    return H2_PAL_OK;
  }
  int result = tcp_socket_connect(
      &transport->tcp_socket, transport->tcp_socket.net,
      &transport->source_addr,
      &transport->remote_addr, timeout_ms);
  transport->tcp_connected = result == H2_PAL_OK;
  return result;
}

int ice_transport_flush(IceTransport* transport, uint32_t timeout_ms) {
  if (transport == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (transport->protocol != ICE_TRANSPORT_TCP ||
      transport->pending_offset == transport->pending_len) {
    transport->pending_len = 0u;
    transport->pending_offset = 0u;
    return H2_PAL_OK;
  }
  if (!transport->tcp_connected) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  size_t remaining = transport->pending_len - transport->pending_offset;
  int sent = tcp_socket_send_timeout(
      &transport->tcp_socket,
      transport->pending_frame + transport->pending_offset,
      (int)remaining,
      timeout_ms);
  if (sent > 0) {
    transport->pending_offset += (size_t)sent;
    if (transport->pending_offset == transport->pending_len) {
      transport->pending_len = 0u;
      transport->pending_offset = 0u;
      return H2_PAL_OK;
    }
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  return sent;
}

int ice_transport_send_packet(
    IceTransport* transport, const uint8_t* packet, size_t packet_len,
    uint32_t timeout_ms) {
  if (transport == NULL || (packet == NULL && packet_len != 0u) ||
      packet_len > CONFIG_MTU || packet_len > UINT16_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (transport->protocol == ICE_TRANSPORT_UDP) {
    return udp_socket_sendto(
        transport->udp_socket, &transport->remote_addr,
        packet, (int)packet_len);
  }
  if (!transport->tcp_connected) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (transport->pending_len != 0u) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  transport->pending_frame[0] = (uint8_t)(packet_len >> 8u);
  transport->pending_frame[1] = (uint8_t)packet_len;
  if (packet_len != 0u) {
    memcpy(transport->pending_frame + 2u, packet, packet_len);
  }
  transport->pending_len = packet_len + 2u;
  transport->pending_offset = 0u;
  int result = ice_transport_flush(transport, timeout_ms);
  if (result == H2_PAL_OK || result == H2_PAL_ERR_WOULD_BLOCK ||
      result == H2_PAL_ERR_TIMEOUT) {
    return (int)packet_len;
  }
  return result;
}

static int ice_transport_tcp_receive(
    IceTransport* transport, uint8_t* out_packet, size_t out_cap,
    uint32_t timeout_ms) {
  int used_wait_budget = 0;
  for (;;) {
    if (transport->receive_prefix_len < 2u) {
      int received = tcp_socket_recv(
          &transport->tcp_socket,
          transport->receive_prefix + transport->receive_prefix_len,
          (int)(2u - transport->receive_prefix_len),
          used_wait_budget ? 0u : timeout_ms);
      used_wait_budget = 1;
      if (received == H2_PAL_ERR_WOULD_BLOCK ||
          received == H2_PAL_ERR_TIMEOUT) {
        return 0;
      }
      if (received < 0) {
        return received;
      }
      transport->receive_prefix_len += (size_t)received;
      if (transport->receive_prefix_len < 2u) {
        return 0;
      }
      transport->receive_expected =
          ((size_t)transport->receive_prefix[0] << 8u) |
          transport->receive_prefix[1];
      if (transport->receive_expected == 0u) {
        ice_transport_reset_receive(transport);
        continue;
      }
      if (transport->receive_expected > sizeof(transport->receive_packet) ||
          transport->receive_expected > out_cap) {
        return H2_PAL_ERR_NO_SPACE;
      }
    }
    int received = tcp_socket_recv(
        &transport->tcp_socket,
        transport->receive_packet + transport->receive_len,
        (int)(transport->receive_expected - transport->receive_len),
        used_wait_budget ? 0u : timeout_ms);
    used_wait_budget = 1;
    if (received == H2_PAL_ERR_WOULD_BLOCK || received == H2_PAL_ERR_TIMEOUT) {
      return 0;
    }
    if (received < 0) {
      return received;
    }
    transport->receive_len += (size_t)received;
    if (transport->receive_len < transport->receive_expected) {
      return 0;
    }
    size_t packet_len = transport->receive_expected;
    memcpy(out_packet, transport->receive_packet, packet_len);
    ice_transport_reset_receive(transport);
    return (int)packet_len;
  }
}

int ice_transport_receive_packet(
    IceTransport* transport, h2_pal_net_addr_t* out_addr,
    uint8_t* out_packet, size_t out_cap, uint32_t timeout_ms) {
  if (transport == NULL || out_packet == NULL || out_cap == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (out_addr != NULL) {
    *out_addr = transport->remote_addr;
  }
  if (transport->protocol == ICE_TRANSPORT_UDP) {
    return udp_socket_recvfrom(
        transport->udp_socket, out_addr, out_packet, (int)out_cap,
        timeout_ms);
  }
  if (!transport->tcp_connected) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  return ice_transport_tcp_receive(
      transport, out_packet, out_cap, timeout_ms);
}

void ice_transport_close(IceTransport* transport) {
  if (transport == NULL) {
    return;
  }
  if (transport->protocol == ICE_TRANSPORT_TCP) {
    tcp_socket_close(&transport->tcp_socket);
  }
  transport->udp_socket = NULL;
  transport->tcp_connected = 0;
  transport->pending_len = 0u;
  transport->pending_offset = 0u;
  ice_transport_reset_receive(transport);
}
