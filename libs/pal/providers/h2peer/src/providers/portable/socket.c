#include "socket.h"

#include <string.h>

int udp_socket_open(UdpSocket* udp_socket, const h2_pal_net_api_t* net,
                    h2_pal_net_family_t family, uint16_t port) {
  if (udp_socket == NULL || net == NULL ||
      !h2_peer_net_addr_family_is_valid(family)) {
    return -1;
  }
  memset(&udp_socket->bind_addr, 0, sizeof(udp_socket->bind_addr));
  udp_socket->fd = -1;
  udp_socket->net = net;
  int result = h2_pal_net_udp_open_bound(
      net, family, port, NULL, &udp_socket->fd, &udp_socket->bind_addr);
  if (result != H2_PAL_OK || udp_socket->bind_addr.family != family) {
    if (udp_socket->fd >= 0) {
      h2_pal_net_close(net, udp_socket->fd);
    }
    udp_socket->fd = -1;
    memset(&udp_socket->bind_addr, 0, sizeof(udp_socket->bind_addr));
    return -1;
  }
  return 0;
}

void udp_socket_close(UdpSocket* udp_socket) {
  if (udp_socket != NULL && udp_socket->fd >= 0) {
    h2_pal_net_close(udp_socket->net, udp_socket->fd);
    udp_socket->fd = -1;
  }
}

int udp_socket_sendto(UdpSocket* udp_socket, const h2_pal_net_addr_t* address,
                      const uint8_t* buf, int len) {
  if (udp_socket == NULL || address == NULL || len < 0 ||
      !h2_peer_net_addr_family_is_valid(address->family)) {
    return -1;
  }
  return h2_pal_net_udp_sendto(udp_socket->net, udp_socket->fd,
                               address, buf, (size_t)len);
}

int udp_socket_recvfrom(UdpSocket* udp_socket, h2_pal_net_addr_t* address,
                        uint8_t* buf, int len, uint32_t timeout_ms) {
  if (udp_socket == NULL || len < 0) {
    return -1;
  }
  if (address != NULL) {
    memset(address, 0, sizeof(*address));
  }
  h2_pal_net_addr_t received_address;
  memset(&received_address, 0, sizeof(received_address));
  int result = h2_pal_net_udp_recvfrom(
      udp_socket->net, udp_socket->fd, &received_address, buf, (size_t)len,
      timeout_ms);
  if (result == H2_PAL_ERR_TIMEOUT || result == H2_PAL_ERR_WOULD_BLOCK) {
    return 0;
  }
  if (result < 0) {
    return -1;
  }
  if (!h2_peer_net_addr_family_is_valid(received_address.family)) {
    return -1;
  }
  if (address != NULL) {
    *address = received_address;
  }
  return result;
}

int tcp_socket_connect(
    TcpSocket* tcp_socket, const h2_pal_net_api_t* net,
    const h2_pal_net_addr_t* source_addr,
    const h2_pal_net_addr_t* remote_addr, uint32_t timeout_ms) {
  if (tcp_socket == NULL || net == NULL || source_addr == NULL ||
      remote_addr == NULL || source_addr->family != remote_addr->family ||
      !h2_peer_net_addr_family_is_valid(source_addr->family)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (tcp_socket->fd < 0) {
    tcp_socket->net = net;
    h2_pal_net_bind_t bind_config;
    memset(&bind_config, 0, sizeof(bind_config));
    bind_config.type = H2_PAL_NET_BIND_SOURCE_ADDR;
    bind_config.source_addr = *source_addr;
    bind_config.source_addr.port = 0u;
    int open_result = h2_pal_net_tcp_open_bound(
        net, source_addr->family, &bind_config, &tcp_socket->fd);
    if (open_result != H2_PAL_OK) {
      tcp_socket->fd = -1;
      return open_result;
    }
  }
  int result = h2_pal_net_tcp_connect(
      net, tcp_socket->fd, remote_addr, timeout_ms);
  if (result != H2_PAL_OK && result != H2_PAL_ERR_TIMEOUT &&
      result != H2_PAL_ERR_WOULD_BLOCK) {
    tcp_socket_close(tcp_socket);
  }
  return result;
}

int tcp_socket_send_timeout(
    TcpSocket* tcp_socket, const uint8_t* buf, int len,
    uint32_t timeout_ms) {
  if (tcp_socket == NULL || tcp_socket->fd < 0 || len < 0 ||
      (buf == NULL && len != 0)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_pal_net_tcp_send_timeout(
      tcp_socket->net, tcp_socket->fd, buf, (size_t)len, timeout_ms);
}

int tcp_socket_recv(
    TcpSocket* tcp_socket, uint8_t* buf, int len, uint32_t timeout_ms) {
  if (tcp_socket == NULL || tcp_socket->fd < 0 || buf == NULL || len <= 0) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_pal_net_tcp_recv(
      tcp_socket->net, tcp_socket->fd, buf, (size_t)len, timeout_ms);
}

void tcp_socket_close(TcpSocket* tcp_socket) {
  if (tcp_socket != NULL && tcp_socket->fd >= 0) {
    h2_pal_net_close(tcp_socket->net, tcp_socket->fd);
    tcp_socket->fd = -1;
  }
}
