#include "h2_esp_alipay_internal.h"
#include "alipay_net_kal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

/* The delivered fd_set ABI has exactly one byte. Give the SDK logical fd 0;
 * never index that byte array with a real lwIP descriptor. SDK calls are
 * serialized by the process adapter, which owns this one socket. */
static int s_socket = -1;
static uint32_t s_connection;
static int descriptor(int fd) { return fd == 0 ? s_socket : -1; }
static int io_result(int value) {
  return value >= 0 ? value
                    : (errno == EAGAIN || errno == EWOULDBLOCK ||
                               errno == EINPROGRESS || errno == EINTR
                           ? -2
                           : -1);
}
void alipay_iot_gettimeofday(alipay_iot_timeval* out) {
  if (out == NULL) return;
  uint64_t ms = 0u;
  if (h2_pal_time_get_wall_ms(h2_esp_alipay_config()->time, &ms) != H2_PAL_OK)
    ms = 0u;
  out->tv_sec = (long)(ms / 1000u);
  out->tv_usec = (long)(ms % 1000u) * 1000;
}
void alipay_task_sleep(uint32_t ms) {
  (void)h2_pal_time_sleep_ms(h2_esp_alipay_config()->time, ms);
}
alipay_iot_mutex alipay_iot_mutex_create(const char* name) {
  (void)name;
  return xSemaphoreCreateMutex();
}
int alipay_iot_mutex_lock(alipay_iot_mutex mutex) {
  return mutex != NULL && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE ? 0
                                                                         : -1;
}
int alipay_iot_mutex_unlock(alipay_iot_mutex mutex) {
  return mutex != NULL && xSemaphoreGive(mutex) == pdTRUE ? 0 : -1;
}
int alipay_iot_mutex_delete(alipay_iot_mutex mutex) {
  if (mutex == NULL) return -1;
  vSemaphoreDelete(mutex);
  return 0;
}
void alipay_iot_fd_zero(alipay_iot_fd_set* set) {
  if (set != NULL) memset(set, 0, sizeof(*set));
}
void alipay_iot_fd_setbit(int fd, alipay_iot_fd_set* set) {
  if (set != NULL && fd == 0) set->fds_bits[0] = 1u;
}
int alipay_iot_fd_isset(int fd, alipay_iot_fd_set* set) {
  return set != NULL && fd == 0 && set->fds_bits[0] != 0u;
}
int alipay_iot_select(int maxfd, alipay_iot_fd_set* readset,
                      alipay_iot_fd_set* writeset, alipay_iot_fd_set* exceptset,
                      alipay_iot_timeval* timeout) {
  if (maxfd != 1 || s_socket < 0) return -1;
  fd_set rd, wr, ex;
  FD_ZERO(&rd);
  FD_ZERO(&wr);
  FD_ZERO(&ex);
  if (readset != NULL && readset->fds_bits[0]) FD_SET(s_socket, &rd);
  if (writeset != NULL && writeset->fds_bits[0]) FD_SET(s_socket, &wr);
  if (exceptset != NULL && exceptset->fds_bits[0]) FD_SET(s_socket, &ex);
  struct timeval tv;
  if (timeout != NULL) {
    if (timeout->tv_sec < 0 || timeout->tv_usec < 0 ||
        timeout->tv_usec >= 1000000)
      return -1;
    tv.tv_sec = timeout->tv_sec;
    tv.tv_usec = timeout->tv_usec;
  }
  int rc = select(s_socket + 1, &rd, &wr, &ex, timeout != NULL ? &tv : NULL);
  if (readset != NULL) readset->fds_bits[0] = rc > 0 && FD_ISSET(s_socket, &rd);
  if (writeset != NULL)
    writeset->fds_bits[0] = rc > 0 && FD_ISSET(s_socket, &wr);
  if (exceptset != NULL)
    exceptset->fds_bits[0] = rc > 0 && FD_ISSET(s_socket, &ex);
  return io_result(rc);
}
int alipay_iot_dns(const char* name, unsigned char ip[4]) {
  if (name == NULL || ip == NULL) return -1;
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo* result = NULL;
  uint64_t started = h2_esp_alipay_now();
  int rc = getaddrinfo(name, NULL, &hints, &result);
  h2_esp_alipay_trace("dns", "resolver", rc, 0, s_connection, started, name,
                      NULL, 0);
  if (rc != 0 || result == NULL) return -1;
  memcpy(ip, &((struct sockaddr_in*)result->ai_addr)->sin_addr.s_addr, 4u);
  char address[24];
  (void)snprintf(address, sizeof(address), "%u.%u.%u.%u", ip[0], ip[1], ip[2],
                 ip[3]);
  h2_esp_alipay_trace("dns.address", "resolver", 0, 0, s_connection, started,
                      address, NULL, 0);
  freeaddrinfo(result);
  return 0;
}
int alipay_iot_socket_create(int domain, alipay_socket_type_enum type,
                             int protocol) {
  if (domain != ALIPAY_IOT_SOC_PF_INET || protocol != 0 || s_socket >= 0 ||
      (type != ALIPAY_IOT_SOC_SOCK_STREAM && type != ALIPAY_IOT_SOC_SOCK_DGRAM))
    return -1;
  int fd =
      socket(AF_INET,
             type == ALIPAY_IOT_SOC_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  const struct timeval timeout = {.tv_sec = 15, .tv_usec = 0};
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
    close(fd);
    return -1;
  }
  s_socket = fd;
  ++s_connection;
  h2_esp_alipay_trace("socket.create", "wire_below_vendor_tls", 0, type,
                      s_connection, h2_esp_alipay_now(), NULL, NULL, 0);
  return 0;
}
int alipay_iot_socket_close(int s) {
  int fd = descriptor(s);
  if (fd < 0) return -1;
  s_socket = -1;
  int rc = close(fd) == 0 ? 0 : -1;
  h2_esp_alipay_trace("socket.close", "wire_below_vendor_tls", rc, 0,
                      s_connection, h2_esp_alipay_now(), NULL, NULL, 0);
  return rc;
}
static int address(const struct alipay_iot_sockaddr* in,
                   struct sockaddr_in* out) {
  if (in == NULL || in->sa_family != ALIPAY_IOT_SOC_PF_INET) return -1;
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_port = in->data.sin_data.port;
  memcpy(&out->sin_addr.s_addr, in->data.sin_data.ip, 4u);
  return 0;
}
int alipay_iot_socket_bind(int s, const struct alipay_iot_sockaddr* name,
                           unsigned int length) {
  (void)length;
  struct sockaddr_in addr;
  int fd = descriptor(s);
  return fd >= 0 && address(name, &addr) == 0 &&
                 bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0
             ? 0
             : -1;
}
int alipay_iot_socket_connect(int s, const struct alipay_iot_sockaddr* name,
                              unsigned int length) {
  (void)length;
  struct sockaddr_in addr;
  int fd = descriptor(s);
  if (fd < 0 || address(name, &addr) != 0) return -1;
  char endpoint[32];
  (void)snprintf(endpoint, sizeof(endpoint), "%u.%u.%u.%u:%u",
                 name->data.sin_data.ip[0], name->data.sin_data.ip[1],
                 name->data.sin_data.ip[2], name->data.sin_data.ip[3],
                 ntohs(addr.sin_port));
  uint64_t started = h2_esp_alipay_now();
  int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
  int error = rc < 0 ? errno : 0;
  rc = io_result(rc);
  h2_esp_alipay_trace("socket.connect", "wire_below_vendor_tls", rc, error,
                      s_connection, started, endpoint, NULL, 0);
  return rc;
}
int alipay_iot_socket_sendto(int s, const void* data, int length, int flags,
                             const struct alipay_iot_sockaddr* to,
                             unsigned int tolen) {
  (void)flags;
  (void)tolen;
  struct sockaddr_in addr;
  int fd = descriptor(s);
  if (fd < 0 || data == NULL || length < 0 || address(to, &addr) != 0)
    return -1;
  return io_result(sendto(fd, data, (size_t)length, 0, (struct sockaddr*)&addr,
                          sizeof(addr)));
}
int alipay_iot_socket_write(int s, const void* data, int length) {
  int fd = descriptor(s);
  if (fd < 0 || data == NULL || length < 0) return -1;
  uint64_t started = h2_esp_alipay_now();
  int rc = send(fd, data, (size_t)length, 0);
  int error = rc < 0 ? errno : 0;
  h2_esp_alipay_trace("socket.send", "wire_below_vendor_tls", rc, error,
                      s_connection, started, "stream_fragment", data,
                      rc > 0 ? (size_t)rc : 0);
  return io_result(rc);
}
int alipay_iot_socket_recvfrom(int s, void* data, int length, int flags,
                               struct alipay_iot_sockaddr* from,
                               unsigned int* fromlen) {
  int fd = descriptor(s);
  struct sockaddr_in addr;
  socklen_t size = sizeof(addr);
  if (fd < 0 || data == NULL || length < 0 || flags != 0) return -1;
  int rc =
      recvfrom(fd, data, (size_t)length, 0, (struct sockaddr*)&addr, &size);
  if (rc >= 0 && from != NULL) {
    memset(from, 0, sizeof(*from));
    from->sa_family = ALIPAY_IOT_SOC_PF_INET;
    from->data.sin_data.port = addr.sin_port;
    memcpy(from->data.sin_data.ip, &addr.sin_addr.s_addr, 4u);
    if (fromlen != NULL) *fromlen = sizeof(*from);
  }
  return io_result(rc);
}
int alipay_iot_socket_read(int s, void* data, int length) {
  int fd = descriptor(s);
  if (fd < 0 || data == NULL || length < 0) return -1;
  uint64_t started = h2_esp_alipay_now();
  int rc = recv(fd, data, (size_t)length, 0);
  int error = rc < 0 ? errno : 0;
  h2_esp_alipay_trace("socket.receive", "wire_below_vendor_tls", rc, error,
                      s_connection, started, "stream_fragment", data,
                      rc > 0 ? (size_t)rc : 0);
  return io_result(rc);
}
int alipay_iot_socket_setsockopt(int s, int level, int option,
                                 const void* value, unsigned int length) {
  (void)level;
  int fd = descriptor(s);
  int val = 0;
  if (fd < 0 || value == NULL || (length != 1u && length != sizeof(int)))
    return -1;
  if (length == 1u)
    val = *(const uint8_t*)value;
  else
    memcpy(&val, value, sizeof(val));
  if (option == ALIPAY_IOT_SOC_NBIO) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1
                     : fcntl(fd, F_SETFL,
                             val ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
  }
  int native;
  switch (option) {
    case ALIPAY_IOT_SOC_NODELAY:
      native = TCP_NODELAY;
      level = IPPROTO_TCP;
      break;
    case ALIPAY_IOT_SOC_KEEPALIVE:
      native = SO_KEEPALIVE;
      level = SOL_SOCKET;
      break;
    case ALIPAY_IOT_SOC_RCVBUF:
      native = SO_RCVBUF;
      level = SOL_SOCKET;
      break;
    case ALIPAY_IOT_SOC_SENDBUF:
      native = SO_SNDBUF;
      level = SOL_SOCKET;
      break;
    default:
      return -1;
  }
  return setsockopt(fd, level, native, &val, sizeof(val)) == 0 ? 0 : -1;
}
int alipay_iot_socket_getsockopt(int s, int level, int option, void* value,
                                 unsigned int* length) {
  (void)s;
  (void)level;
  (void)option;
  (void)value;
  (void)length;
  return -1;
}
