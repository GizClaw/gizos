#include "h2_iostreamikcp_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct h2_iostreamikcp {
    h2_iostreamikcp_config_t config;
    h2_iostreamikcp_filter_t filter;
    ikcpcb *kcp;
    uint8_t *rx_buffer;
    size_t rx_size;
    size_t rx_head;
    size_t rx_len;
    uint8_t *frame_buffer;
    size_t frame_buffer_size;
    h2_iostreamikcp_stats_t stats;
    h2_pal_result_t last_output_error;
};

static void *stream_alloc(const h2_pal_mem_api_t *allocator, size_t len) {
    if (allocator != NULL) {
        return h2_pal_mem_alloc(allocator, len);
    }
    return malloc(len);
}

static void stream_free(const h2_pal_mem_api_t *allocator, void *ptr) {
    if (ptr == NULL) {
        return;
    }
    if (allocator != NULL) {
        h2_pal_mem_free(allocator, ptr);
    } else {
        free(ptr);
    }
}

static void stream_record_output_error(h2_iostreamikcp_t *stream, h2_pal_result_t rc) {
    if (stream->last_output_error == H2_PAL_OK) {
        stream->last_output_error = rc;
    }
}

static uint32_t stream_now_ms(h2_iostreamikcp_t *stream) {
    if (stream->config.now_ms != NULL) {
        return stream->config.now_ms(stream->config.time_user);
    }
    return 0u;
}

static int stream_output(const char *buf, int len, ikcpcb *kcp, void *user) {
    (void)kcp;
    h2_iostreamikcp_t *stream = (h2_iostreamikcp_t *)user;
    /* KCP keeps calling output for the remaining window even if output
     * returns -1. Bound one update/flush to one failed transport write rather
     * than paying the full I/O timeout again for every queued segment. */
    if (stream->last_output_error != H2_PAL_OK) {
        return -1;
    }
    h2_iostreamikcp_frame_t frame = {
        .flags = H2_IOSTREAMIKCP_FRAME_FLAG_DATA,
        .conv = stream->config.conv,
        .payload = (const uint8_t *)buf,
        .payload_len = len < 0 ? 0u : (size_t)len,
    };
    size_t frame_len = 0u;
    size_t written = 0u;
    h2_pal_result_t rc = h2_iostreamikcp_frame_encode(
        &frame,
        stream->frame_buffer,
        stream->frame_buffer_size,
        &frame_len);
    if (rc != H2_PAL_OK) {
        stream->stats.input_errors++;
        stream_record_output_error(stream, rc);
        return -1;
    }
    rc = stream->config.io.write(
        stream->config.io.user,
        stream->frame_buffer,
        frame_len,
        &written,
        stream->config.write_timeout_ms);
    if (rc != H2_PAL_OK || written != frame_len) {
        stream->stats.input_errors++;
        stream_record_output_error(stream, rc != H2_PAL_OK ? rc : H2_PAL_ERR_IO);
        return -1;
    }
    stream->stats.tx_frames++;
    stream->stats.tx_bytes += frame.payload_len;
    return 0;
}

static h2_pal_result_t stream_append_rx(h2_iostreamikcp_t *stream, const uint8_t *data, size_t len) {
    if (len > stream->rx_size - stream->rx_len) {
        return H2_PAL_ERR_NO_SPACE;
    }
    for (size_t i = 0u; i < len; ++i) {
        size_t index = (stream->rx_head + stream->rx_len) % stream->rx_size;
        stream->rx_buffer[index] = data[i];
        stream->rx_len++;
    }
    if (stream->rx_len > stream->stats.rx_high_water) {
        stream->stats.rx_high_water = stream->rx_len;
    }
    stream->stats.rx_bytes += len;
    return H2_PAL_OK;
}

static h2_pal_result_t stream_drain_kcp(h2_iostreamikcp_t *stream) {
    for (;;) {
        int n = ikcp_peeksize(stream->kcp);
        if (n < 0) {
            return H2_PAL_OK;
        }
        if ((size_t)n > stream->rx_size - stream->rx_len) {
            return H2_PAL_OK;
        }

        uint8_t stack_scratch[512];
        uint8_t *scratch = stack_scratch;
        if ((size_t)n > sizeof(stack_scratch)) {
            scratch = (uint8_t *)stream_alloc(stream->config.allocator, (size_t)n);
            if (scratch == NULL) {
                return H2_PAL_ERR_NO_MEMORY;
            }
        }
        int got = ikcp_recv(stream->kcp, (char *)scratch, n);
        if (got < 0) {
            if (scratch != stack_scratch) {
                stream_free(stream->config.allocator, scratch);
            }
            stream->stats.input_errors++;
            return H2_PAL_ERR_IO;
        }
        h2_pal_result_t rc = stream_append_rx(stream, scratch, (size_t)got);
        if (scratch != stack_scratch) {
            stream_free(stream->config.allocator, scratch);
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
}

static h2_pal_result_t stream_on_frame(void *user, const h2_iostreamikcp_frame_t *frame) {
    h2_iostreamikcp_t *stream = (h2_iostreamikcp_t *)user;
    if (frame->flags != H2_IOSTREAMIKCP_FRAME_FLAG_DATA ||
        frame->conv != stream->config.conv) {
        return H2_PAL_OK;
    }
    return h2_iostreamikcp_input_frame(stream, frame);
}

h2_pal_result_t h2_iostreamikcp_input_frame(
    h2_iostreamikcp_t *stream,
    const h2_iostreamikcp_frame_t *frame) {
    if (stream == NULL || frame == NULL ||
        frame->flags != H2_IOSTREAMIKCP_FRAME_FLAG_DATA ||
        frame->conv != stream->config.conv ||
        frame->payload == NULL || frame->payload_len == 0u ||
        frame->payload_len > H2_IOSTREAMIKCP_MAX_PAYLOAD_LEN) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = ikcp_input(stream->kcp, (const char *)frame->payload, (long)frame->payload_len);
    if (rc < 0) {
        stream->stats.input_errors++;
        return H2_PAL_ERR_FORMAT;
    }
    stream->stats.rx_frames++;
    return stream_drain_kcp(stream);
}

static h2_pal_result_t uart_io_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    return h2_pal_uart_io_stream_read(
        (const h2_pal_uart_io_stream_api_t *)user,
        buffer,
        len,
        out_read,
        timeout_ms);
}

static h2_pal_result_t uart_io_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    return h2_pal_uart_io_stream_write(
        (const h2_pal_uart_io_stream_api_t *)user,
        buffer,
        len,
        out_written,
        timeout_ms);
}

static h2_pal_result_t uart_io_flush(void *user) {
    return h2_pal_uart_io_stream_flush((const h2_pal_uart_io_stream_api_t *)user);
}

static h2_pal_result_t usb_jtag_io_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    return h2_pal_usb_jtag_io_stream_read(
        (const h2_pal_usb_jtag_io_stream_api_t *)user,
        buffer,
        len,
        out_read,
        timeout_ms);
}

static h2_pal_result_t usb_jtag_io_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    return h2_pal_usb_jtag_io_stream_write(
        (const h2_pal_usb_jtag_io_stream_api_t *)user,
        buffer,
        len,
        out_written,
        timeout_ms);
}

static h2_pal_result_t usb_jtag_io_flush(void *user) {
    return h2_pal_usb_jtag_io_stream_flush((const h2_pal_usb_jtag_io_stream_api_t *)user);
}

h2_iostreamikcp_io_t h2_iostreamikcp_io_from_uart(const h2_pal_uart_io_stream_api_t *api) {
    h2_iostreamikcp_io_t io = {
        .user = (void *)api,
        .read = uart_io_read,
        .write = uart_io_write,
        .flush = uart_io_flush,
    };
    return io;
}

h2_iostreamikcp_io_t h2_iostreamikcp_io_from_usb_jtag(const h2_pal_usb_jtag_io_stream_api_t *api) {
    h2_iostreamikcp_io_t io = {
        .user = (void *)api,
        .read = usb_jtag_io_read,
        .write = usb_jtag_io_write,
        .flush = usb_jtag_io_flush,
    };
    return io;
}

h2_pal_result_t h2_iostreamikcp_open(
    const h2_iostreamikcp_config_t *config,
    h2_iostreamikcp_t **out_stream) {
    h2_iostreamikcp_t *stream;
    size_t mtu;
    size_t rx_size;
    uint32_t receive_window;

    if (out_stream != NULL) {
        *out_stream = NULL;
    }
    if (config == NULL || out_stream == NULL || config->io.write == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    mtu = config->mtu == 0u ? H2_IOSTREAMIKCP_DEFAULT_MTU : config->mtu;
    rx_size = config->rx_buffer_size == 0u ? 4096u : config->rx_buffer_size;
    receive_window = config->receive_window == 0u ?
        H2_IOSTREAMIKCP_DEFAULT_RECEIVE_WINDOW : config->receive_window;
    if (mtu > H2_IOSTREAMIKCP_MAX_PAYLOAD_LEN || mtu < 64u || rx_size < mtu ||
        receive_window < H2_IOSTREAMIKCP_MIN_RECEIVE_WINDOW ||
        receive_window > H2_IOSTREAMIKCP_MAX_RECEIVE_WINDOW) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    stream = (h2_iostreamikcp_t *)stream_alloc(config->allocator, sizeof(*stream));
    if (stream == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(stream, 0, sizeof(*stream));
    stream->config = *config;
    stream->config.conv = config->conv == 0u ? H2_IOSTREAMIKCP_DEFAULT_CONV : config->conv;
    stream->config.mtu = mtu;
    stream->config.rx_buffer_size = rx_size;
    stream->config.receive_window = receive_window;
    stream->rx_size = rx_size;
    stream->frame_buffer_size = h2_iostreamikcp_frame_encoded_len(mtu);
    stream->rx_buffer = (uint8_t *)stream_alloc(config->allocator, rx_size);
    stream->frame_buffer = (uint8_t *)stream_alloc(config->allocator, stream->frame_buffer_size);
    stream->kcp = ikcp_create(stream->config.conv, stream);
    if (stream->rx_buffer == NULL || stream->frame_buffer == NULL || stream->kcp == NULL) {
        h2_iostreamikcp_close(stream);
        return H2_PAL_ERR_NO_MEMORY;
    }
    ikcp_setoutput(stream->kcp, stream_output);
    if (ikcp_nodelay(stream->kcp, 1, 10, 2, 0) != 0 ||
        ikcp_wndsize(stream->kcp, 64, 64) != 0 ||
        ikcp_setmtu(stream->kcp, (int)mtu) != 0) {
        h2_iostreamikcp_close(stream);
        return H2_PAL_ERR_INVALID_ARG;
    }
    /* Upstream clamps rcv_wnd to 128. This stream contract bounds writes to
     * at most two fragments, so a smaller transport-sized window is valid. */
    stream->kcp->rcv_wnd = receive_window;
    stream->kcp->rmt_wnd = H2_IOSTREAMIKCP_DEFAULT_RECEIVE_WINDOW;
    h2_iostreamikcp_filter_init(&stream->filter);
    *out_stream = stream;
    return H2_PAL_OK;
}

void h2_iostreamikcp_close(h2_iostreamikcp_t *stream) {
    if (stream == NULL) {
        return;
    }
    const h2_pal_mem_api_t *allocator = stream->config.allocator;
    if (stream->kcp != NULL) {
        ikcp_release(stream->kcp);
    }
    stream_free(allocator, stream->rx_buffer);
    stream_free(allocator, stream->frame_buffer);
    stream_free(allocator, stream);
}

h2_pal_result_t h2_iostreamikcp_input(
    h2_iostreamikcp_t *stream,
    const uint8_t *data,
    size_t len) {
    if (stream == NULL || (len > 0u && data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_iostreamikcp_filter_input_with_log(
        &stream->filter, data, len, stream_on_frame, stream,
        stream->config.on_log, stream->config.log_user);
    stream->stats.input_log_bytes = stream->filter.log_bytes;
    stream->stats.crc_errors = stream->filter.crc_errors;
    return rc;
}

h2_pal_result_t h2_iostreamikcp_poll(
    h2_iostreamikcp_t *stream,
    uint32_t timeout_ms) {
    if (stream == NULL || stream->config.io.read == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint8_t buffer[256];
    size_t out_read = 0u;
    h2_pal_result_t rc = stream->config.io.read(
        stream->config.io.user,
        buffer,
        sizeof(buffer),
        &out_read,
        timeout_ms);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_WOULD_BLOCK) {
        return rc;
    }
    if (out_read == 0u) {
        return rc == H2_PAL_OK ? H2_PAL_ERR_TIMEOUT : rc;
    }
    return h2_iostreamikcp_input(stream, buffer, out_read);
}

h2_pal_result_t h2_iostreamikcp_update(h2_iostreamikcp_t *stream, uint32_t now_ms) {
    if (stream == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    stream->last_output_error = H2_PAL_OK;
    ikcp_update(stream->kcp, now_ms);
    if (stream->last_output_error != H2_PAL_OK) {
        return stream->last_output_error;
    }
    stream->stats.waitsnd = (uint32_t)ikcp_waitsnd(stream->kcp);
    return stream_drain_kcp(stream);
}

h2_pal_result_t h2_iostreamikcp_read(
    h2_iostreamikcp_t *stream,
    uint8_t *out,
    size_t out_size,
    size_t *out_len) {
    size_t count;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (stream == NULL || out == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (stream->rx_len == 0u) {
        h2_pal_result_t rc = stream_drain_kcp(stream);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (stream->rx_len == 0u) {
            return H2_PAL_ERR_WOULD_BLOCK;
        }
    }
    count = stream->rx_len < out_size ? stream->rx_len : out_size;
    for (size_t i = 0u; i < count; ++i) {
        out[i] = stream->rx_buffer[stream->rx_head];
        stream->rx_head = (stream->rx_head + 1u) % stream->rx_size;
        stream->rx_len--;
    }
    *out_len = count;
    (void)stream_drain_kcp(stream);
    return H2_PAL_OK;
}

h2_pal_result_t h2_iostreamikcp_write(
    h2_iostreamikcp_t *stream,
    const uint8_t *data,
    size_t len) {
    if (stream == NULL || (len > 0u && data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return H2_PAL_OK;
    }
    while (len > 0u) {
        size_t mss = (size_t)stream->kcp->mss;
        size_t chunk = len < mss ? len : mss;
        if (chunk > (size_t)INT_MAX) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        if (ikcp_send(stream->kcp, (const char *)data, (int)chunk) < 0) {
            return H2_PAL_ERR_IO;
        }
        data += chunk;
        len -= chunk;
    }
    return h2_iostreamikcp_flush(stream);
}

h2_pal_result_t h2_iostreamikcp_flush(h2_iostreamikcp_t *stream) {
    if (stream == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    stream->last_output_error = H2_PAL_OK;
    ikcp_update(stream->kcp, stream_now_ms(stream));
    ikcp_flush(stream->kcp);
    if (stream->last_output_error == H2_PAL_OK && stream->config.io.flush != NULL) {
        stream->last_output_error = stream->config.io.flush(stream->config.io.user);
    }
    stream->stats.waitsnd = (uint32_t)ikcp_waitsnd(stream->kcp);
    return stream->last_output_error;
}

h2_pal_result_t h2_iostreamikcp_get_stats(
    h2_iostreamikcp_t *stream,
    h2_iostreamikcp_stats_t *out_stats) {
    if (stream == NULL || out_stats == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_iostreamikcp_stats_t stats = stream->stats;
    stats.input_log_bytes = stream->filter.log_bytes;
    stats.input_errors += stream->filter.errors;
    stats.crc_errors = stream->filter.crc_errors;
    stats.waitsnd = (uint32_t)ikcp_waitsnd(stream->kcp);
    *out_stats = stats;
    return H2_PAL_OK;
}
