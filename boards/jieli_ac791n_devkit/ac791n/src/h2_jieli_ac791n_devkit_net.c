#include "asm/includes.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2/pal/net/h2_pal_net.h"

#ifdef H2_JIELI_NETWORK_ENABLE

#include "h2_jieli_wl82_atomic.h"
#include "h2_jieli_ac791n_devkit_network.h"

#include "lwip.h"
#include "lwip/inet.h"
#include "lwip/api.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_net_resolver {
  unsigned references;
  unsigned slot;
  h2_pal_result_t result;
  h2_pal_net_addr_t address;
  char host[DNS_MAX_NAME_LENGTH];
};

enum { H2_JIELI_DNS_CAPACITY = 4 };
static h2_pal_net_resolver_t *resolvers[H2_JIELI_DNS_CAPACITY];
static uint32_t stack_gate;
static int stack_ready;
static uint32_t stack_generation;
static uint32_t stack_users;
enum { SLOT_RECV = 1, SLOT_SEND = 2, SLOT_CONNECT = 4 };
static struct {
  uint8_t busy;
  uint32_t generation;
} sockets[MEMP_NUM_NETCONN];

static void stack_lock(void) {
  for (;;) {
    uint32_t expected = 0u;
    if (h2_jieli_atomic_cas_u32(&stack_gate, &expected, 1u)) return;
    os_time_dly(1u);
  }
}

static void stack_unlock(void) {
  h2_jieli_atomic_store_u32(&stack_gate, 0u);
}

void h2_jieli_net_stack_started(void) {
  stack_lock();
  if (++stack_generation == 0u) ++stack_generation;
  stack_ready = 1;
  stack_unlock();
}

void h2_jieli_net_stack_stopping(void) {
  stack_lock();
  stack_ready = 0;
  while (stack_users != 0u) {
    stack_unlock();
    os_time_dly(1u);
    stack_lock();
  }
  stack_unlock();
}

static int stack_enter(void) {
  stack_lock();
  int result = stack_ready ? H2_PAL_OK : H2_PAL_ERR_UNAVAILABLE;
  if (result == H2_PAL_OK) ++stack_users;
  stack_unlock();
  return result;
}

static void stack_leave(void) {
  stack_lock();
  --stack_users;
  stack_unlock();
}

static int slot_enter(int fd, uint8_t bit) {
  if (fd < LWIP_SOCKET_OFFSET || fd - LWIP_SOCKET_OFFSET >= MEMP_NUM_NETCONN) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  unsigned index = (unsigned)(fd - LWIP_SOCKET_OFFSET);
  stack_lock();
  int result = H2_PAL_OK;
  if (!stack_ready || (sockets[index].generation != 0u &&
                      sockets[index].generation != stack_generation)) {
    result = H2_PAL_ERR_UNAVAILABLE;
  } else if (bit != 0u && (sockets[index].busy &
             (bit == SLOT_CONNECT ? (SLOT_RECV | SLOT_SEND | SLOT_CONNECT) :
                                    (bit | SLOT_CONNECT))) != 0u) {
    result = H2_PAL_ERR_BUSY;
  } else {
    sockets[index].busy |= bit;
    ++stack_users;
  }
  stack_unlock();
  return result;
}

static void slot_leave(int fd, uint8_t bit) {
  stack_lock();
  sockets[fd - LWIP_SOCKET_OFFSET].busy &= (uint8_t)~bit;
  --stack_users;
  stack_unlock();
}

static void slot_opened(int fd) {
  stack_lock();
  sockets[fd - LWIP_SOCKET_OFFSET].generation = stack_generation;
  sockets[fd - LWIP_SOCKET_OFFSET].busy = 0u;
  stack_unlock();
}

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

static h2_pal_result_t set_timeout(int socket_fd, int option, uint32_t timeout_ms) {
  struct timeval timeout = {
      .tv_sec = (long)(timeout_ms / 1000u),
      .tv_usec = (long)((timeout_ms % 1000u) * 1000u),
  };
  return setsockopt(
      socket_fd, SOL_SOCKET, option, &timeout, (socklen_t)sizeof(timeout)) == 0
      ? H2_PAL_OK : map_socket_error();
}

static int resolve_addr(
    void *user, const char *host, h2_pal_net_addr_t *out_addr) {
  (void)user;
  if (host == NULL || out_addr == NULL) return H2_PAL_ERR_INVALID_ARG;
  int result = stack_enter();
  if (result != H2_PAL_OK) return result;
  ip_addr_t address;
  err_t error = netconn_gethostbyname_addrtype(host, &address, NETCONN_DNS_IPV4);
  if (error == ERR_OK) {
    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->family = H2_PAL_NET_FAMILY_IPV4;
    memcpy(out_addr->ip, &ip_2_ip4(&address)->addr, 4u);
  }
  stack_leave();
  return error == ERR_OK ? H2_PAL_OK :
      (error == ERR_VAL || error == ERR_ARG ? H2_PAL_ERR_NOT_FOUND : H2_PAL_ERR_IO);
}

static void resolver_release(h2_pal_net_resolver_t *resolver) {
  if (__atomic_sub_fetch(&resolver->references, 1u, __ATOMIC_ACQ_REL) == 0u) {
    free(resolver);
  }
}

void h2_jieli_net_stack_stopped(void) {
  h2_pal_net_resolver_t *settled[H2_JIELI_DNS_CAPACITY] = {0};
  stack_lock();
  for (unsigned i = 0; i < H2_JIELI_DNS_CAPACITY; ++i) {
    settled[i] = resolvers[i];
    if (settled[i] != NULL) {
      __atomic_store_n(&settled[i]->result, H2_PAL_ERR_UNAVAILABLE, __ATOMIC_RELEASE);
      resolvers[i] = NULL;
    }
  }
  stack_unlock();
  for (unsigned i = 0; i < H2_JIELI_DNS_CAPACITY; ++i) {
    if (settled[i] != NULL) resolver_release(settled[i]);
  }
}

static void resolver_complete(
    void *user, const ip_addr_t *address, h2_pal_result_t result) {
  stack_lock();
  h2_pal_net_resolver_t *resolver = NULL;
  for (unsigned i = 0; i < H2_JIELI_DNS_CAPACITY; ++i) {
    if (resolvers[i] == user) {
      resolver = resolvers[i];
      resolvers[i] = NULL;
      break;
    }
  }
  if (resolver != NULL) {
    if (address != NULL && IP_IS_V4(address)) {
      resolver->address.family = H2_PAL_NET_FAMILY_IPV4;
      memcpy(resolver->address.ip, &ip_2_ip4(address)->addr, 4u);
      result = H2_PAL_OK;
    }
    __atomic_store_n(&resolver->result, result, __ATOMIC_RELEASE);
  }
  stack_unlock();
  if (resolver != NULL) resolver_release(resolver);
}

static void resolver_found(const char *name, const ip_addr_t *address, void *user) {
  (void)name;
  resolver_complete(user, address, H2_PAL_ERR_NOT_FOUND);
}

/* Raw lwIP DNS APIs belong to the TCP/IP thread. Its reference survives an
 * early caller close and is released by either immediate or delayed completion. */
static void resolver_begin(void *user) {
  stack_lock();
  h2_pal_net_resolver_t *resolver = NULL;
  for (unsigned i = 0; i < H2_JIELI_DNS_CAPACITY; ++i) {
    if (resolvers[i] == user) resolver = resolvers[i];
  }
  if (resolver == NULL) {
    stack_unlock();
    return;
  }
  ip_addr_t address;
  const err_t result = dns_gethostbyname_addrtype(
      resolver->host, &address, resolver_found, resolver, LWIP_DNS_ADDRTYPE_IPV4);
  stack_unlock();
  if (result == ERR_OK) {
    resolver_found(NULL, &address, resolver);
  } else if (result != ERR_INPROGRESS) {
    resolver_complete(resolver, NULL,
                      result == ERR_MEM ? H2_PAL_ERR_NO_SPACE : H2_PAL_ERR_IO);
  }
}

static h2_pal_result_t resolve_start(
    void *user, const char *host, h2_pal_net_resolver_t **out_resolver) {
  (void)user;
  if (host == NULL || out_resolver == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_resolver = NULL;
  const size_t length = strnlen(host, DNS_MAX_NAME_LENGTH);
  if (length == 0u || length == DNS_MAX_NAME_LENGTH) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int result = stack_enter();
  if (result != H2_PAL_OK) return result;
  stack_lock();
  unsigned slot = 0u;
  while (slot < H2_JIELI_DNS_CAPACITY && resolvers[slot] != NULL) ++slot;
  if (slot == H2_JIELI_DNS_CAPACITY) {
    stack_unlock();
    stack_leave();
    return H2_PAL_ERR_NO_SPACE;
  }
  h2_pal_net_resolver_t *resolver = malloc(sizeof(*resolver));
  if (resolver == NULL) {
    stack_unlock();
    stack_leave();
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(resolver, 0, sizeof(*resolver));
  resolver->references = 2u;
  resolver->slot = slot;
  resolver->result = H2_PAL_ERR_WOULD_BLOCK;
  memcpy(resolver->host, host, length + 1u);
  resolvers[slot] = resolver;
  stack_unlock();
  if (tcpip_try_callback(resolver_begin, resolver) != ERR_OK) {
    resolver_complete(resolver, NULL, H2_PAL_ERR_NO_SPACE);
    resolver_release(resolver);
    stack_leave();
    return H2_PAL_ERR_NO_SPACE;
  }
  stack_leave();
  *out_resolver = resolver;
  return H2_PAL_OK;
}

static h2_pal_result_t resolve_poll(
    void *user, h2_pal_net_resolver_t *resolver,
    h2_pal_net_addr_t *out_addr, uint32_t timeout_ms) {
  (void)user;
  if (resolver == NULL || out_addr == NULL) return H2_PAL_ERR_INVALID_ARG;
  const uint32_t start = timer_get_ms();
  for (;;) {
    const h2_pal_result_t result =
        __atomic_load_n(&resolver->result, __ATOMIC_ACQUIRE);
    if (result != H2_PAL_ERR_WOULD_BLOCK) {
      if (result == H2_PAL_OK) *out_addr = resolver->address;
      return result;
    }
    if (timeout_ms == 0u) return H2_PAL_ERR_WOULD_BLOCK;
    const uint32_t elapsed = (uint32_t)(timer_get_ms() - start);
    /* A WL82 scheduler tick is 10 ms; do not round a shorter budget upward. */
    if (elapsed >= timeout_ms || timeout_ms - elapsed < 10u) {
      return H2_PAL_ERR_TIMEOUT;
    }
    os_time_dly(1u);
  }
}

static void resolve_close(void *user, h2_pal_net_resolver_t *resolver) {
  (void)user;
  if (resolver != NULL) resolver_release(resolver);
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

static int udp_open_bound_active(
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

static int udp_open_bound(
    void *user, h2_pal_net_family_t family, uint16_t port,
    const h2_pal_net_bind_t *binding, h2_pal_net_socket_t *out_socket,
    h2_pal_net_addr_t *out_bind_addr) {
  int result = stack_enter();
  if (result != H2_PAL_OK) return result;
  result = udp_open_bound_active(
      user, family, port, binding, out_socket, out_bind_addr);
  if (result == H2_PAL_OK) slot_opened(*out_socket);
  stack_leave();
  return result;
}

static int udp_open(
    void *user, h2_pal_net_family_t family, uint16_t port,
    h2_pal_net_socket_t *out_socket, h2_pal_net_addr_t *out_bind_addr) {
  return udp_open_bound(
      user, family, port, NULL, out_socket, out_bind_addr);
}

static int udp_sendto_active(
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

static int udp_sendto(
    void *user, h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *address, const uint8_t *data, size_t length) {
  int result = slot_enter(socket_fd, SLOT_SEND);
  if (result != H2_PAL_OK) return result;
  result = udp_sendto_active(user, socket_fd, address, data, length);
  slot_leave(socket_fd, SLOT_SEND);
  return result;
}

static int udp_recvfrom_active(
    void *user, h2_pal_net_socket_t socket_fd, h2_pal_net_addr_t *out_addr,
    uint8_t *data, size_t length, uint32_t timeout_ms) {
  (void)user;
  if (out_addr == NULL || data == NULL || length == 0u || length > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int result = set_timeout(socket_fd, SO_RCVTIMEO, timeout_ms);
  if (result != H2_PAL_OK) return result;
  struct sockaddr_in native;
  socklen_t native_length = sizeof(native);
  int received = recvfrom(
      socket_fd, data, length, timeout_ms == 0u ? MSG_DONTWAIT : 0,
      (struct sockaddr *)&native, &native_length);
  if (received < 0) return map_timed_socket_error(timeout_ms);
  sockaddr_to_addr(&native, out_addr);
  return received;
}

static int udp_recvfrom(
    void *user, h2_pal_net_socket_t socket_fd, h2_pal_net_addr_t *out_addr,
    uint8_t *data, size_t length, uint32_t timeout_ms) {
  int result = slot_enter(socket_fd, SLOT_RECV);
  if (result != H2_PAL_OK) return result;
  result = udp_recvfrom_active(user, socket_fd, out_addr, data, length, timeout_ms);
  slot_leave(socket_fd, SLOT_RECV);
  return result;
}

static int udp_join_multicast_active(
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

static int udp_join_multicast(
    void *user, h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *address) {
  int result = slot_enter(socket_fd, SLOT_SEND);
  if (result != H2_PAL_OK) return result;
  result = udp_join_multicast_active(user, socket_fd, address);
  slot_leave(socket_fd, SLOT_SEND);
  return result;
}

static int tcp_open_active(
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

static int tcp_open(
    void *user, h2_pal_net_family_t family,
    h2_pal_net_socket_t *out_socket) {
  int result = stack_enter();
  if (result != H2_PAL_OK) return result;
  result = tcp_open_active(user, family, out_socket);
  if (result == H2_PAL_OK) slot_opened(*out_socket);
  stack_leave();
  return result;
}

static int tcp_open_bound_active(
    void *user, h2_pal_net_family_t family,
    const h2_pal_net_bind_t *binding, h2_pal_net_socket_t *out_socket) {
  int result = tcp_open_active(user, family, out_socket);
  if (result != H2_PAL_OK) return result;
  result = bind_socket(*out_socket, binding == NULL ? 0u : binding->source_addr.port,
                       binding, NULL);
  if (result != H2_PAL_OK) {
    closesocket(*out_socket);
    *out_socket = -1;
  }
  return result;
}

static int tcp_open_bound(
    void *user, h2_pal_net_family_t family,
    const h2_pal_net_bind_t *binding, h2_pal_net_socket_t *out_socket) {
  int result = stack_enter();
  if (result != H2_PAL_OK) return result;
  result = tcp_open_bound_active(user, family, binding, out_socket);
  if (result == H2_PAL_OK) slot_opened(*out_socket);
  stack_leave();
  return result;
}

static int tcp_connect_active(
    void *user, h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *address, uint32_t timeout_ms) {
  (void)user;
  struct sockaddr_in native;
  int result = addr_to_sockaddr(address, &native);
  if (result != H2_PAL_OK) return result;
  /* Preserve the caller's mode on terminal failures, including retries of
   * an earlier nonblocking connect. TIMEOUT/WOULD_BLOCK remain pending. */
  int original_flags = fcntl(socket_fd, F_GETFL, 0);
  if (original_flags < 0) return map_socket_error();
  unsigned long nonblocking = 1u;
  if (ioctlsocket(socket_fd, FIONBIO, &nonblocking) != 0) {
    return map_socket_error();
  }
  if (connect(socket_fd, (struct sockaddr *)&native, sizeof(native)) == 0 ||
      errno == EISCONN) {
    nonblocking = 0u;
    return ioctlsocket(socket_fd, FIONBIO, &nonblocking) == 0
               ? H2_PAL_OK : map_socket_error();
  }
  if (errno != EINPROGRESS && errno != EALREADY && errno != EAGAIN &&
      errno != EWOULDBLOCK) {
    result = map_socket_error();
    goto terminal_error;
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
  if (selected < 0) {
    result = map_socket_error();
    goto terminal_error;
  }
  int socket_error = 0;
  socklen_t error_length = sizeof(socket_error);
  if (getsockopt(
          socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0) {
    result = map_socket_error();
    goto terminal_error;
  }
  if (socket_error != 0) {
    errno = socket_error;
    result = map_socket_error();
    goto terminal_error;
  }
  nonblocking = 0u;
  return ioctlsocket(socket_fd, FIONBIO, &nonblocking) == 0
             ? H2_PAL_OK : map_socket_error();

terminal_error:
  nonblocking = (original_flags & O_NONBLOCK) != 0;
  return ioctlsocket(socket_fd, FIONBIO, &nonblocking) == 0
             ? result : map_socket_error();
}

static int tcp_connect(
    void *user, h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *address, uint32_t timeout_ms) {
  int result = slot_enter(socket_fd, SLOT_CONNECT);
  if (result != H2_PAL_OK) return result;
  result = tcp_connect_active(user, socket_fd, address, timeout_ms);
  slot_leave(socket_fd, SLOT_CONNECT);
  return result;
}

static int tcp_send_timeout_active(
    void *user, h2_pal_net_socket_t socket_fd, const uint8_t *data,
    size_t length, uint32_t timeout_ms) {
  (void)user;
  if ((data == NULL && length != 0u) || length > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int result = set_timeout(socket_fd, SO_SNDTIMEO, timeout_ms);
  if (result != H2_PAL_OK) return result;
  int sent = send(
      socket_fd, data, length, timeout_ms == 0u ? MSG_DONTWAIT : 0);
  return sent < 0 ? map_timed_socket_error(timeout_ms) : sent;
}

static int tcp_send_timeout(
    void *user, h2_pal_net_socket_t socket_fd, const uint8_t *data,
    size_t length, uint32_t timeout_ms) {
  int result = slot_enter(socket_fd, SLOT_SEND);
  if (result != H2_PAL_OK) return result;
  result = tcp_send_timeout_active(user, socket_fd, data, length, timeout_ms);
  slot_leave(socket_fd, SLOT_SEND);
  return result;
}

static int tcp_send(
    void *user, h2_pal_net_socket_t socket_fd,
    const uint8_t *data, size_t length) {
  return tcp_send_timeout(user, socket_fd, data, length, 0u);
}

static int tcp_recv_active(
    void *user, h2_pal_net_socket_t socket_fd, uint8_t *data,
    size_t length, uint32_t timeout_ms) {
  (void)user;
  if (data == NULL || length == 0u || length > INT_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int result = set_timeout(socket_fd, SO_RCVTIMEO, timeout_ms);
  if (result != H2_PAL_OK) return result;
  int received = recv(
      socket_fd, data, length, timeout_ms == 0u ? MSG_DONTWAIT : 0);
  if (received == 0) return H2_PAL_ERR_CLOSED;
  return received < 0 ? map_timed_socket_error(timeout_ms) : received;
}

static int tcp_recv(
    void *user, h2_pal_net_socket_t socket_fd, uint8_t *data,
    size_t length, uint32_t timeout_ms) {
  int result = slot_enter(socket_fd, SLOT_RECV);
  if (result != H2_PAL_OK) return result;
  result = tcp_recv_active(user, socket_fd, data, length, timeout_ms);
  slot_leave(socket_fd, SLOT_RECV);
  return result;
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

static int tcp_listen(
    void *user, h2_pal_net_family_t family, uint16_t port,
    const h2_pal_net_bind_t *bind, h2_pal_net_socket_t *out_socket,
    h2_pal_net_addr_t *out_bind_addr) {
  (void)user;
  (void)family;
  (void)port;
  (void)bind;
  (void)out_socket;
  (void)out_bind_addr;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t tcp_accept(
    void *user, h2_pal_net_socket_t listen_socket,
    h2_pal_net_socket_t *out_socket, h2_pal_net_addr_t *out_peer_addr,
    uint32_t timeout_ms) {
  (void)user;
  (void)listen_socket;
  (void)out_socket;
  (void)out_peer_addr;
  (void)timeout_ms;
  return H2_PAL_ERR_UNSUPPORTED;
}

static void close_socket(void *user, h2_pal_net_socket_t socket_fd) {
  (void)user;
  if (slot_enter(socket_fd, 0u) != H2_PAL_OK) return;
  closesocket(socket_fd);
  stack_lock();
  sockets[socket_fd - LWIP_SOCKET_OFFSET].generation = 0u;
  sockets[socket_fd - LWIP_SOCKET_OFFSET].busy = 0u;
  stack_unlock();
  stack_leave();
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
      .tcp_listen = tcp_listen,
      .tcp_accept = tcp_accept,
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
