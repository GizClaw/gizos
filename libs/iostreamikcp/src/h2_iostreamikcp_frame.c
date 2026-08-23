#include "h2_iostreamikcp_internal.h"

#include <string.h>

const uint8_t h2_iostreamikcp_frame_magic[H2_IOSTREAMIKCP_FRAME_MAGIC_LEN] = {
    'H', '2', 'I', 'K', 'C', 'P',
};

uint16_t h2_iostreamikcp_read_le16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

uint32_t h2_iostreamikcp_read_le32(const uint8_t *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

void h2_iostreamikcp_write_le16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
}

void h2_iostreamikcp_write_le32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

uint32_t h2_iostreamikcp_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0u; i < len; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static int frame_flags_valid(uint8_t flags) {
    return flags == H2_IOSTREAMIKCP_FRAME_FLAG_DATA ||
           flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN ||
           flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK;
}

static int frame_control_valid(const h2_iostreamikcp_frame_t *frame) {
    if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_DATA) {
        return 1;
    }
    return frame->conv != 0u &&
           frame->payload_len == H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN &&
           frame->payload != NULL &&
           h2_iostreamikcp_read_le32(frame->payload) == frame->conv;
}

size_t h2_iostreamikcp_frame_encoded_len(size_t payload_len) {
    return H2_IOSTREAMIKCP_FRAME_HEADER_LEN + payload_len;
}

h2_pal_result_t h2_iostreamikcp_frame_encode(
    const h2_iostreamikcp_frame_t *frame,
    uint8_t *out,
    size_t out_size,
    size_t *out_len) {
    size_t needed;
    uint32_t crc;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (frame == NULL || out == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!frame_flags_valid(frame->flags) ||
        frame->payload_len > H2_IOSTREAMIKCP_MAX_PAYLOAD_LEN ||
        (frame->payload_len > 0u && frame->payload == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!frame_control_valid(frame)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    needed = h2_iostreamikcp_frame_encoded_len(frame->payload_len);
    if (out_size < needed) {
        return H2_PAL_ERR_NO_SPACE;
    }

    memcpy(out, h2_iostreamikcp_frame_magic, H2_IOSTREAMIKCP_FRAME_MAGIC_LEN);
    out[6] = H2_IOSTREAMIKCP_FRAME_VERSION;
    out[7] = frame->flags;
    h2_iostreamikcp_write_le32(out + 8u, frame->conv);
    h2_iostreamikcp_write_le16(out + H2_IOSTREAMIKCP_FRAME_LEN_OFFSET, (uint16_t)frame->payload_len);
    crc = h2_iostreamikcp_crc32(frame->payload, frame->payload_len);
    h2_iostreamikcp_write_le32(out + 14u, crc);
    if (frame->payload_len > 0u) {
        memcpy(out + H2_IOSTREAMIKCP_FRAME_HEADER_LEN, frame->payload, frame->payload_len);
    }
    *out_len = needed;
    return H2_PAL_OK;
}

void h2_iostreamikcp_filter_init(h2_iostreamikcp_filter_t *filter) {
    if (filter != NULL) {
        memset(filter, 0, sizeof(*filter));
    }
}

static int filter_prefix_matches(const uint8_t *buffer, size_t len) {
    if (len <= H2_IOSTREAMIKCP_FRAME_MAGIC_LEN) {
        return memcmp(buffer, h2_iostreamikcp_frame_magic, len) == 0;
    }
    return memcmp(buffer, h2_iostreamikcp_frame_magic, H2_IOSTREAMIKCP_FRAME_MAGIC_LEN) == 0;
}

static void filter_drop_one(h2_iostreamikcp_filter_t *filter) {
    if (filter->len == 0u) {
        return;
    }
    memmove(filter->buffer, filter->buffer + 1u, filter->len - 1u);
    filter->len--;
}

static void filter_drop(h2_iostreamikcp_filter_t *filter, size_t len) {
    if (len > filter->len) {
        len = filter->len;
    }
    memmove(filter->buffer, filter->buffer + len, filter->len - len);
    filter->len -= len;
}

static h2_pal_result_t filter_try_emit(
    h2_iostreamikcp_filter_t *filter,
    h2_iostreamikcp_frame_fn on_frame,
    void *user) {
    h2_iostreamikcp_frame_t frame;
    uint16_t payload_len;
    size_t total_len;
    uint32_t want_crc;
    uint32_t got_crc;

    if (filter->len < H2_IOSTREAMIKCP_FRAME_HEADER_LEN) {
        return H2_PAL_OK;
    }
    if (filter->buffer[6] != H2_IOSTREAMIKCP_FRAME_VERSION ||
        !frame_flags_valid(filter->buffer[7])) {
        filter->errors++;
        filter_drop_one(filter);
        return H2_PAL_OK;
    }
    payload_len = h2_iostreamikcp_read_le16(filter->buffer + H2_IOSTREAMIKCP_FRAME_LEN_OFFSET);
    if (payload_len > H2_IOSTREAMIKCP_MAX_PAYLOAD_LEN) {
        filter->errors++;
        filter->resyncing = 1;
        filter_drop_one(filter);
        return H2_PAL_OK;
    }
    total_len = H2_IOSTREAMIKCP_FRAME_HEADER_LEN + (size_t)payload_len;
    if (filter->len < total_len) {
        return H2_PAL_OK;
    }
    want_crc = h2_iostreamikcp_read_le32(filter->buffer + 14u);
    got_crc = h2_iostreamikcp_crc32(filter->buffer + H2_IOSTREAMIKCP_FRAME_HEADER_LEN, payload_len);
    if (want_crc != got_crc) {
        filter->crc_errors++;
        filter->resyncing = 1;
        filter_drop_one(filter);
        return H2_PAL_OK;
    }

    frame.flags = filter->buffer[7];
    frame.conv = h2_iostreamikcp_read_le32(filter->buffer + 8u);
    frame.payload = filter->buffer + H2_IOSTREAMIKCP_FRAME_HEADER_LEN;
    frame.payload_len = payload_len;
    if (!frame_control_valid(&frame)) {
        filter->errors++;
        filter->resyncing = 1;
        filter_drop_one(filter);
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = H2_PAL_OK;
    if (on_frame != NULL) {
        rc = on_frame(user, &frame);
    }
    filter->frames++;
    filter_drop(filter, total_len);
    return rc;
}

static h2_pal_result_t filter_process(
    h2_iostreamikcp_filter_t *filter,
    h2_iostreamikcp_frame_fn on_frame,
    void *user,
    h2_iostreamikcp_log_fn on_log,
    void *log_user) {
    for (;;) {
        if (filter->resyncing) {
            while (filter->len > 0u && !filter_prefix_matches(filter->buffer, filter->len)) {
                filter_drop_one(filter);
            }
            if (filter->len < H2_IOSTREAMIKCP_FRAME_MAGIC_LEN) {
                break;
            }
            filter->resyncing = 0;
        }
        while (filter->len > 0u && !filter_prefix_matches(filter->buffer, filter->len)) {
            if (on_log != NULL) {
                h2_pal_result_t log_rc = on_log(log_user, filter->buffer, 1u);
                if (log_rc != H2_PAL_OK) {
                    return log_rc;
                }
            }
            filter->log_bytes++;
            filter_drop_one(filter);
        }
        if (filter->len < H2_IOSTREAMIKCP_FRAME_HEADER_LEN) {
            break;
        }
        size_t old_len = filter->len;
        h2_pal_result_t rc = filter_try_emit(filter, on_frame, user);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (filter->len == old_len) {
            break;
        }
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_iostreamikcp_filter_input(
    h2_iostreamikcp_filter_t *filter,
    const uint8_t *data,
    size_t len,
    h2_iostreamikcp_frame_fn on_frame,
    void *user) {
    return h2_iostreamikcp_filter_input_with_log(
        filter, data, len, on_frame, user, NULL, NULL);
}

h2_pal_result_t h2_iostreamikcp_filter_input_with_log(
    h2_iostreamikcp_filter_t *filter,
    const uint8_t *data,
    size_t len,
    h2_iostreamikcp_frame_fn on_frame,
    void *frame_user,
    h2_iostreamikcp_log_fn on_log,
    void *log_user) {
    if (filter == NULL || (len > 0u && data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < len; ++i) {
        if (filter->len == sizeof(filter->buffer)) {
            h2_pal_result_t rc = filter_process(
                filter, on_frame, frame_user, on_log, log_user);
            if (rc != H2_PAL_OK) {
                return rc;
            }
        }
        if (filter->len == sizeof(filter->buffer)) {
            filter->errors++;
            filter_drop_one(filter);
        }
        filter->buffer[filter->len++] = data[i];
    }
    return filter_process(filter, on_frame, frame_user, on_log, log_user);
}
