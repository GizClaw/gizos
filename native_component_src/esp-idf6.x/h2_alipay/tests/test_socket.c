#include <arpa/inet.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "h2_esp_alipay_internal.h"
#include "h2_esp_alipay_socket_abi.h"

static char endpoint[64];
uint64_t h2_esp_alipay_now(void) { return 0; }
void h2_esp_alipay_trace(const char* operation, const char* boundary,
                         int result, uint32_t item, uint32_t connection,
                         uint64_t started, const char* detail,
                         const void* bytes, size_t length) {
  (void)boundary;
  (void)result;
  (void)item;
  (void)connection;
  (void)started;
  (void)bytes;
  (void)length;
  if (strcmp(operation, "socket.connect") == 0)
    snprintf(endpoint, sizeof(endpoint), "%s", detail);
}
int main(void) {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  struct sockaddr_in local = {.sin_family = AF_INET,
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(bind(listener, (struct sockaddr*)&local, sizeof(local)) == 0);
  assert(listen(listener, 1) == 0);
  socklen_t size = sizeof(local);
  assert(getsockname(listener, (struct sockaddr*)&local, &size) == 0);
  struct alipay_iot_sockaddr peer = {.sa_family = ALIPAY_IOT_SOC_PF_INET};
  peer.data.sin_data.port = ntohs(local.sin_port);
  memcpy(peer.data.sin_data.ip, &local.sin_addr.s_addr, 4);
  assert(alipay_iot_socket_create(ALIPAY_IOT_SOC_PF_INET,
                                  ALIPAY_IOT_SOC_SOCK_STREAM, 0) == 0);
  assert(alipay_iot_socket_create(ALIPAY_IOT_SOC_PF_INET,
                                  ALIPAY_IOT_SOC_SOCK_STREAM, 0) == -1);
  int yes = 1;
  assert(alipay_iot_socket_setsockopt(0, 0, ALIPAY_IOT_SOC_NBIO, &yes,
                                      sizeof(yes)) == 0);
  int rc = alipay_iot_socket_connect(0, &peer, sizeof(peer));
  assert(rc == 0 || rc == -2);
  char expected[64];
  snprintf(expected, sizeof(expected), "127.0.0.1:%u", ntohs(local.sin_port));
  assert(strcmp(endpoint, expected) == 0);
  int remote = accept(listener, NULL, NULL);
  assert(remote >= 0);
  alipay_iot_fd_set rd;
  alipay_iot_fd_zero(&rd);
  alipay_iot_fd_setbit(0, &rd);
  assert(!alipay_iot_fd_isset(0, &rd));
  alipay_iot_timeval timeout = {0};
  assert(alipay_iot_select(1, &rd, NULL, NULL, &timeout) == 0);
  assert(!alipay_iot_fd_isset(0, &rd));
  char byte;
  assert(alipay_iot_socket_read(0, &byte, 1) == -2);
  assert(send(remote, "A", 1, 0) == 1);
  timeout.tv_sec = 2;
  /* A timeout must preserve the requested bit for the next select. */
  assert(alipay_iot_select(1, &rd, NULL, NULL, &timeout) == 1);
  assert(alipay_iot_fd_isset(0, &rd));
  assert(alipay_iot_socket_read(0, &byte, 1) == 1 && byte == 'A');
  timeout.tv_sec = 0;
  assert(alipay_iot_select(1, &rd, NULL, NULL, &timeout) == 0);
  assert(!alipay_iot_fd_isset(0, &rd));
  assert(alipay_iot_socket_write(0, "B", 1) == 1);
  assert(recv(remote, &byte, 1, 0) == 1 && byte == 'B');
  close(remote);
  timeout.tv_sec = 2;
  assert(alipay_iot_select(1, &rd, NULL, NULL, &timeout) == 1);
  assert(alipay_iot_socket_read(0, &byte, 1) == 0);
  assert(alipay_iot_socket_close(0) == 0);
  assert(alipay_iot_socket_close(0) == -1);
  close(listener);
  puts("socket port, readiness, retry, stream and close checks passed");
  return 0;
}
