#ifndef H2_ESP_PLATFORM_NET_SOCKET_H
#define H2_ESP_PLATFORM_NET_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

int h2_esp_net_wait_fd(int fd, int write_ready, uint32_t timeout_ms);
int h2_esp_net_udp_send_result(int sent, int error_code);

/**
 * Apply a bounded receive timeout. lwIP treats SO_RCVTIMEO 0 as "block
 * forever", so callers must never pass 0 here; they use MSG_DONTWAIT instead.
 * Returns H2_PAL_OK or H2_PAL_ERR_IO when the option cannot be applied, in
 * which case the caller must not issue a blocking receive.
 */
int h2_esp_net_set_recv_timeout(int fd, uint32_t timeout_ms);

/**
 * Receive from a plain (non-TLS) stream socket with PAL timeout semantics:
 * timeout 0 polls with MSG_DONTWAIT and reports H2_PAL_ERR_WOULD_BLOCK,
 * a positive timeout waits at most that long and reports H2_PAL_ERR_TIMEOUT,
 * an orderly peer close reports H2_PAL_ERR_CLOSED, other failures
 * H2_PAL_ERR_CLOSED (reset) or H2_PAL_ERR_IO. Success returns bytes read.
 */
int h2_esp_net_stream_recv(int fd, uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * Receive one datagram with the same timeout semantics as
 * h2_esp_net_stream_recv(); `addr`/`addr_len` may be NULL.
 */
int h2_esp_net_datagram_recv(
    int fd,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms,
    struct sockaddr *addr,
    socklen_t *addr_len);

#endif
