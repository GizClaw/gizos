#ifndef H2_IOSTREAMIKCP_TYPES_H
#define H2_IOSTREAMIKCP_TYPES_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_IOSTREAMIKCP_DEFAULT_CONV UINT32_C(0x49324b43)
#define H2_IOSTREAMIKCP_DEFAULT_MTU 352u
#define H2_IOSTREAMIKCP_DEFAULT_RECEIVE_WINDOW 64u
#define H2_IOSTREAMIKCP_MIN_RECEIVE_WINDOW 2u
#define H2_IOSTREAMIKCP_MAX_RECEIVE_WINDOW 128u
#define H2_IOSTREAMIKCP_MAX_PAYLOAD_LEN 1024u
#define H2_IOSTREAMIKCP_FRAME_HEADER_LEN 18u
#define H2_IOSTREAMIKCP_FRAME_VERSION 1u
#define H2_IOSTREAMIKCP_FRAME_FLAG_DATA 0x00u
#define H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN 0x01u
#define H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK 0x02u
#define H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN 4u
#define H2_IOSTREAMIKCP_FILTER_BUFFER_SIZE \
    (H2_IOSTREAMIKCP_FRAME_HEADER_LEN + H2_IOSTREAMIKCP_MAX_PAYLOAD_LEN)

typedef struct h2_iostreamikcp h2_iostreamikcp_t;

typedef h2_pal_result_t (*h2_iostreamikcp_read_fn)(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms);

typedef h2_pal_result_t (*h2_iostreamikcp_write_fn)(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms);

typedef h2_pal_result_t (*h2_iostreamikcp_flush_fn)(void *user);

typedef uint32_t (*h2_iostreamikcp_now_ms_fn)(void *user);

/** Consume borrowed non-frame bytes synchronously while polling the stream. */
typedef h2_pal_result_t (*h2_iostreamikcp_log_fn)(
    void *user,
    const uint8_t *data,
    size_t len);

typedef struct h2_iostreamikcp_io {
    void *user;
    h2_iostreamikcp_read_fn read;
    h2_iostreamikcp_write_fn write;
    h2_iostreamikcp_flush_fn flush;
} h2_iostreamikcp_io_t;

typedef struct h2_iostreamikcp_config {
    h2_iostreamikcp_io_t io;
    const h2_pal_mem_api_t *allocator;
    h2_iostreamikcp_now_ms_fn now_ms;
    void *time_user;
    uint32_t conv;
    size_t mtu;
    size_t rx_buffer_size;
    /** Segment capacity advertised to the peer; zero selects the default. */
    uint32_t receive_window;
    uint32_t write_timeout_ms;
    h2_iostreamikcp_log_fn on_log;
    void *log_user;
} h2_iostreamikcp_config_t;

typedef struct h2_iostreamikcp_stats {
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t tx_frames;
    uint64_t rx_frames;
    uint64_t input_log_bytes;
    uint64_t input_errors;
    uint64_t crc_errors;
    size_t rx_high_water;
    uint32_t waitsnd;
} h2_iostreamikcp_stats_t;

typedef struct h2_iostreamikcp_frame {
    uint8_t flags;
    uint32_t conv;
    const uint8_t *payload;
    size_t payload_len;
} h2_iostreamikcp_frame_t;

typedef h2_pal_result_t (*h2_iostreamikcp_frame_fn)(
    void *user,
    const h2_iostreamikcp_frame_t *frame);

typedef struct h2_iostreamikcp_filter {
    uint8_t buffer[H2_IOSTREAMIKCP_FILTER_BUFFER_SIZE];
    size_t len;
    int resyncing;
    uint64_t log_bytes;
    uint64_t frames;
    uint64_t errors;
    uint64_t crc_errors;
} h2_iostreamikcp_filter_t;

#ifdef __cplusplus
}
#endif

#endif
