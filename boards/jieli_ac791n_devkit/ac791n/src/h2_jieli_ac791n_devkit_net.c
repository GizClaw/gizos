#include "asm/includes.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2/pal/net/h2_pal_net.h"

#ifdef H2_JIELI_NETWORK_ENABLE

#include "lwip.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_net_resolver {
  h2_pal_result_t result;
  h2_pal_net_addr_t address;
};

static h2_pal_result_t map_socket_error(void) {
  switch (errno) {
    case EAGAIN:
#ifdef EWOULDBLOCK
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
#endif
      return H2_PAL_ERR_WOULD_BLOCK;
    case ETIMEDOUT: return H2_PAL_ERR_TIMEOUT;
    case ECONNRESET:
    case ENOTCONN:
    case EPIPE: return H2_PAL_ERR_CLOSED;
    case ENOMEM: return H2_PAL_ERR_NO_MEMORY;
    default: return H2_PAL_ERR_IO;
  }
}

static h2_pal_result_t map_timed_socket_error(uint32_t timeout_ms) {
  h2_pal_result_t result = map_socket_error();
  return result == H2_PAL_ERR_WOULD_BLOCK && timeout_ms != 0u
             ? H2_PAL_ERR_TIMEOUT
             : result;
}

static h2_pal_result_t addr_to_sockaddr(
    const h2_pal_net_addr_t *address, struct sockaddr_in *out) {
  if (address == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (address->family != H2_PAL_NET_FAMILY_IPV4) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_port = htons(address->port);
  memcpy(&out->sin_addr.s_addr, address->ip, 4u);
  return H2_PAL_OK;
}

static void sockaddr_to_addr(
    const struct sockaddr_in *address, h2_pal_net_addr_t *out) {
  memset(out, 0, sizeof(*out));
  out->family = H2_PAL_NET_FAMILY_IPV4;
  out->port = ntohs(address->sin_port);
  memcpy(out->ip, &address->sin_addr.s_addr, 4u);
}

static void set_timeout(int socket_fd, int option, uint32_t timeout_ms) {
  struct timeval timeout = {
      .tv_sec = (long)(timeout_ms / 1000u),
      .tv_usec = (long)((timeout_ms % 1000u) * 1000u),
  };
  (void)setsockopt(
      socket_fd, SOL_SOCKET, option, &timeout, (socklen_t)sizeof(timeout));
}

static int resolve_addr(
    void *user, const char *host, h2_pal_net_addr_t *out_addr) {
  (void)user;
  if (host == NULL || out_addr == NULL) return H2_PAL_ERR_INVALID_ARG;
  struct hostent *resolved = gethostbyname(host);
  if (resolved == NULL || resolved->h_addrtype != AF_INET ||
      resolved->h_length != 4 || resolved->h_addr_list == NULL ||
      resolved->h_addr_list[0] == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  memset(out_addr, 0, sizeof(*out_addr));
  out_addr->family = H2_PAL_NET_FAMILY_IPV4;
  memcpy(out_addr->ip, resolved->h_addr_list[0], 4u);
  return H2_PAL_OK;
}

static h2_pal_result_t resolve_start(
    void *user, const char *host, h2_pal_net_resolver_t **out_resolver) {
  if (host == NULL || out_resolver == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_resolver = NULL;
  h2_pal_net_resolver_t *resolver = malloc(sizeof(*resolver));
  if (resolver == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(resolver, 0, sizeof(*resolver));
  resolver->result = resolve_addr(user, host, &resolver->address);
  *out_resolver = resolver;
  return H2_PAL_OK;
}

static h2_pal_result_t resolve_poll(
    void *user, h2_pal_net_resolver_t *resolver,
    h2_pal_net_addr_t *out_addr, uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  if (resolver == NULL || out_addr == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (resolver->result == H2_PAL_OK) *out_addr = resolver->address;
  return resolver->result;
}

static void resolve_close(void *user, h2_pal_net_resolver_t *resolver) {
  (void)user;
  free(resolver);
}

static int get_host_addr(
    void *user, const char *iface_prefix, h2_pal_net_addr_t *out_addr) {
  (void)user;
  (void)iface_prefix;
  if (out_addr == NULL) return H2_PAL_ERR_INVALID_ARG;
  char bytes[4] = {0};
  Get_IPAddress(WIFI_NETIF, bytes);
  memset(out_addr, 0, sizeof(*out_addr));
  out_addr->family = H2_PAL_NET_FAMILY_IPV4;
  memcpy(out_addr->ip, bytes, sizeof(bytes));
  return H2_PAL_OK;
}

static int bind_socket(
    int socket_fd, uint16_t port, const h2_pal_net_bind_t *binding,
    h2_pal_net_addr_t *out_bind_addr) {
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
  if (binding != NULL && binding->type == H2_PAL_NET_BIND_SOURCE_ADDR) {
    h2_pal_result_t result = addr_to_sockaddr(&binding->source_addr, &address);
    if (result != H2_PAL_OK) return result;
    address.sin_port = htons(port);
  } else if (binding != NULL &&
             binding->type != H2_PAL_NET_BIND_DEFAULT) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    return map_socket_error();
  }
  if (out_bind_addr != NULL) {
    socklen_t length = sizeof(address);
    if (getsockname(socket_fd, (struct sockaddr *)&address, &length) != 0) {
      return map_socket_error();
    }
    sockaddr_to_addr(&address, out_bind_addr);
  }
  return H2_PAL_OK;
}

static int udp_open_bound(
    void *user, h2_pal_net_family_t family, uint16_t port,
    const h2_pal_net_bind_t *binding, h2_pal_net_socket_t *out_socket,
    h2_pal_net_addr_t *out_bind_addr) {
  (void)user;
  if (out_socket == NULL || out_bind_addr == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (family != H2_PAL_NET_FAMILY_IPV4) return H2_PAL_ERR_UNSUPPORTED;
  int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0) return map_socket_error();
  int result = bind_socket(socket_fd, port, binding, out_bind_addr);
  if (result != H2_PAL_OK) {
    closesocket(socket_fd);
    return result;
  }
  *out_socket = socket_fd;
  return H2_PAL_OK;
}

static int udp_open(
    void *user, h2_pal_net_family_t family, uint16_t port,
    h2_pal_net_socket_t *out_socket, h2_pal_net_addr_t *out_bind_addr) {
  return udp_open_bound(
      user, family, port, NULL, out_socket, out_bind_addr);
}

static int udp_sendto(
    void *user, h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *address, const uint8_t *data, size_t length) {
  (void)user;
  if (address == NULL || (data == NULL && length != 0u) || length > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  struct sockaddr_in native;
  int result = addr_to_sockaddr(address, &native);
  if (result != H2_PAL_OK) return result;
  int sent = sendto(
      socket_fd, data, length, 0, (struct sockaddr *)&native, sizeof(native));
  return sent < 0 ? map_socket_error() : sent;
}

static int udp_recvfrom(
    void *user, h2_pal_net_socket_t socket_fd, h2_pal_net_addr_t *out_addr,
    uint8_t *data, size_t length, uint32_t timeout_ms) {
  (void)user;
  if (out_addr == NULL || data == NULL || length == 0u || length > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  set_timeout(socket_fd, SO_RCVTIMEO, timeout_ms);
  struct sockaddr_in native;
  socklen_t native_length = sizeof(native);
  int received = recvfrom(
      socket_fd, data, length, timeout_ms == 0u ? MSG_DONTWAIT : 0,
      (struct sockaddr *)&native, &native_length);
  if (received < 0) return map_timed_socket_error(timeout_ms);
  sockaddr_to_addr(&native, out_addr);
  return received;
}

static int udp_join_multicast(
    void *user, h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *address) {
  (void)user;
  if (address == NULL || address->family != H2_PAL_NET_FAMILY_IPV4) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  struct ip_mreq membership;
  memcpy(&membership.imr_multiaddr.s_addr, address->ip, 4u);
  membership.imr_interface.s_addr = PP_HTONL(INADDR_ANY);
  return setsockopt(
             socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
             (socklen_t)sizeof(membership)) == 0
             ? H2_PAL_OK
             : map_socket_error();
}

static int tcp_open(
    void *user, h2_pal_net_family_t family,
    h2_pal_net_socket_t *out_socket) {
  (void)user;
  if (out_socket == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (family != H2_PAL_NET_FAMILY_IPV4) return H2_PAL_ERR_UNSUPPORTED;
  int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_fd < 0) return map_socket_error();
  *out_socket = socket_fd;
  return H2_PAL_OK;
}

static int tcp_open_bound(
    void *user, h2_pal_net_family_t family,
    const h2_pal_net_bind_t *binding, h2_pal_net_socket_t *out_socket) {
  int result = tcp_open(user, family, out_socket);
  if (result != H2_PAL_OK) return result;
  result = bind_socket(*out_socket, binding == NULL ? 0u : binding->source_addr.port,
                       binding, NULL);
  if (result != H2_PAL_OK) {
    closesocket(*out_socket);
    *out_socket = -1;
  }
  return result;
}

static int tcp_connect(
    void *user, h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *address, uint32_t timeout_ms) {
  (void)user;
  struct sockaddr_in native;
  int result = addr_to_sockaddr(address, &native);
  if (result != H2_PAL_OK) return result;
  unsigned long nonblocking = 1u;
  if (ioctlsocket(socket_fd, FIONBIO, &nonblocking) != 0) {
    return map_socket_error();
  }
  if (connect(socket_fd, (struct sockaddr *)&native, sizeof(native)) == 0 ||
      errno == EISCONN) {
    nonblocking = 0u;
    (void)ioctlsocket(socket_fd, FIONBIO, &nonblocking);
    return H2_PAL_OK;
  }
  if (errno != EINPROGRESS && errno != EALREADY && errno != EAGAIN &&
      errno != EWOULDBLOCK) {
    return map_socket_error();
  }
  fd_set writable;
  fd_set failed;
  FD_ZERO(&writable);
  FD_ZERO(&failed);
  FD_SET(socket_fd, &writable);
  FD_SET(socket_fd, &failed);
  struct timeval timeout = {
      .tv_sec = (long)(timeout_ms / 1000u),
      .tv_usec = (long)((timeout_ms % 1000u) * 1000u),
  };
  int selected = select(
      socket_fd + 1, NULL, &writable, &failed, &timeout);
  if (selected == 0) {
    return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
  }
  if (selected < 0) return map_socket_error();
  int socket_error = 0;
  socklen_t error_length = sizeof(socket_error);
  if (getsockopt(
          socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0) {
    return map_socket_error();
  }
  if (socket_error != 0) {
    errno = socket_error;
    return map_socket_error();
  }
  nonblocking = 0u;
  (void)ioctlsocket(socket_fd, FIONBIO, &nonblocking);
  return H2_PAL_OK;
}

static int tcp_send_timeout(
    void *user, h2_pal_net_socket_t socket_fd, const uint8_t *data,
    size_t length, uint32_t timeout_ms) {
  (void)user;
  if ((data == NULL && length != 0u) || length > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  set_timeout(socket_fd, SO_SNDTIMEO, timeout_ms);
  int sent = send(
      socket_fd, data, length, timeout_ms == 0u ? MSG_DONTWAIT : 0);
  return sent < 0 ? map_timed_socket_error(timeout_ms) : sent;
}

static int tcp_send(
    void *user, h2_pal_net_socket_t socket_fd,
    const uint8_t *data, size_t length) {
  return tcp_send_timeout(user, socket_fd, data, length, 0u);
}

static int tcp_recv(
    void *user, h2_pal_net_socket_t socket_fd, uint8_t *data,
    size_t length, uint32_t timeout_ms) {
  (void)user;
  if (data == NULL || length == 0u || length > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  set_timeout(socket_fd, SO_RCVTIMEO, timeout_ms);
  int received = recv(
      socket_fd, data, length, timeout_ms == 0u ? MSG_DONTWAIT : 0);
  if (received == 0) return H2_PAL_ERR_CLOSED;
  return received < 0 ? map_timed_socket_error(timeout_ms) : received;
}

static h2_pal_result_t tls_wrap(
    void *user, h2_pal_net_socket_t tcp_socket,
    const h2_pal_net_tls_config_t *config, uint32_t timeout_ms,
    h2_pal_net_socket_t *out_tls_socket) {
  (void)user;
  (void)tcp_socket;
  (void)config;
  (void)timeout_ms;
  if (out_tls_socket != NULL) *out_tls_socket = -1;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t icmp_echo(
    void *user, const h2_pal_net_addr_t *address,
    const h2_pal_net_bind_t *binding, uint32_t timeout_ms,
    h2_pal_net_icmp_echo_result_t *out_result) {
  (void)user;
  (void)address;
  (void)binding;
  (void)timeout_ms;
  (void)out_result;
  return H2_PAL_ERR_UNSUPPORTED;
}

static void close_socket(void *user, h2_pal_net_socket_t socket_fd) {
  (void)user;
  closesocket(socket_fd);
}

const h2_pal_net_api_t *h2_jieli_ac791n_devkit_net_api(void) {
  static const h2_pal_net_vtable_t vtable = {
      .resolve_addr = resolve_addr,
      .resolve_start = resolve_start,
      .resolve_poll = resolve_poll,
      .resolve_close = resolve_close,
      .get_host_addr = get_host_addr,
      .udp_open = udp_open,
      .udp_sendto = udp_sendto,
      .udp_recvfrom = udp_recvfrom,
      .udp_open_bound = udp_open_bound,
      .udp_join_multicast = udp_join_multicast,
      .tcp_open = tcp_open,
      .tcp_open_bound = tcp_open_bound,
      .tcp_connect = tcp_connect,
      .tcp_send = tcp_send,
      .tcp_send_timeout = tcp_send_timeout,
      .tcp_recv = tcp_recv,
      .tls_wrap = tls_wrap,
      .icmp_echo = icmp_echo,
      .close = close_socket,
  };
  static const h2_pal_net_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}

#else

const h2_pal_net_api_t *h2_jieli_ac791n_devkit_net_api(void) {
  return NULL;
}

#endif
