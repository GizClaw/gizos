#ifndef H2_ESP_PLATFORM_NET_SOCKET_H
#define H2_ESP_PLATFORM_NET_SOCKET_H

#include <stdint.h>

int h2_esp_net_wait_fd(int fd, int write_ready, uint32_t timeout_ms);
int h2_esp_net_udp_send_result(int sent, int error_code);

#endif
