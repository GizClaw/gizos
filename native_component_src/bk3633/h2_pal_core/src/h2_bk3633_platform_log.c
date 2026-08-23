#include "h2_bk3633_platform_core.h"

#include <stdint.h>
#include <string.h>

#ifndef H2_BK3633_LOG_RING_SIZE
#define H2_BK3633_LOG_RING_SIZE 512u
#endif

#ifndef H2_BK3633_LOG_FORMAT_SIZE
#define H2_BK3633_LOG_FORMAT_SIZE 320u
#endif

_Static_assert(H2_BK3633_LOG_FORMAT_SIZE >= 9u,
               "BK3633 log format capacity is too small");

static uint8_t s_ring[H2_BK3633_LOG_RING_SIZE];
static uint16_t s_head;
static uint16_t s_tail;
static uint32_t s_dropped;
static uint8_t s_in_write;
static h2_bk3633_platform_log_config_t s_sink;

static uint16_t ring_next(uint16_t index)
{
    index++;
    return index >= (uint16_t)H2_BK3633_LOG_RING_SIZE ? 0u : index;
}

static int ring_empty(void)
{
    return s_head == s_tail;
}

static int ring_full(void)
{
    return ring_next(s_head) == s_tail;
}

static size_t ring_push(const uint8_t *data, size_t length)
{
    size_t i;

    if (data == NULL) {
        return 0u;
    }
    for (i = 0u; i < length; ++i) {
        if (ring_full()) {
            s_dropped += (uint32_t)(length - i);
            return i;
        }
        s_ring[s_head] = data[i];
        s_head = ring_next(s_head);
    }
    return length;
}

static size_t log_bounded_strlen(const char *text, size_t limit)
{
    size_t length = 0u;
    while (length < limit && text[length] != '\0') {
        ++length;
    }
    return length;
}

static int log_push_segment(const char *text, size_t length,
                            size_t *remaining)
{
    if (length > *remaining) {
        length = *remaining;
    }
    *remaining -= length;
    if (length == 0u) {
        return 1;
    }
    return ring_push((const uint8_t *)text, length) == length;
}

h2_pal_result_t h2_bk3633_platform_log_init(
    const h2_bk3633_platform_log_config_t *config)
{
    if (config == NULL || config->write == NULL ||
        config->max_write_size == 0u ||
        config->max_write_size >= H2_BK3633_LOG_RING_SIZE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_sink = *config;
    s_head = 0u;
    s_tail = 0u;
    s_dropped = 0u;
    s_in_write = 0u;
    return H2_PAL_OK;
}

void h2_bk3633_platform_log_deinit(void)
{
    memset(&s_sink, 0, sizeof(s_sink));
    s_head = 0u;
    s_tail = 0u;
    s_dropped = 0u;
    s_in_write = 0u;
}

h2_pal_result_t h2_bk3633_platform_log_drain(void)
{
    size_t available;
    size_t requested;
    size_t written;

    (void)s_dropped;
    if (s_sink.write == NULL || s_sink.max_write_size == 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (ring_empty()) {
        return H2_PAL_OK;
    }

    available = s_head > s_tail
        ? (size_t)(s_head - s_tail)
        : H2_BK3633_LOG_RING_SIZE - s_tail;
    requested = available < s_sink.max_write_size
        ? available
        : s_sink.max_write_size;
    written = s_sink.write(s_sink.user, &s_ring[s_tail], requested);
    if (written > requested) {
        return H2_PAL_ERR_WRITE;
    }
    for (size_t i = 0u; i < written; ++i) {
        s_tail = ring_next(s_tail);
    }
    return written == requested ? H2_PAL_OK : H2_PAL_ERR_WRITE;
}

static int log_write(void *user, h2_pal_log_level_t level, const char *scope,
                     const char *message)
{
    const char *level_name;
    const char *scope_name;
    size_t scope_length;
    size_t message_length;
    size_t formatted_length;
    size_t remaining;
    const char *line_ending;
    size_t line_ending_length;
    int write_ok = 1;
    int result = H2_PAL_LOG_OK;

    (void)user;
    if (message == NULL || s_in_write != 0u) {
        return message == NULL ? H2_PAL_LOG_ERR_INVALID_ARG : H2_PAL_LOG_OK;
    }

    switch (level) {
    case H2_PAL_LOG_DEBUG: level_name = "D"; break;
    case H2_PAL_LOG_INFO:  level_name = "I"; break;
    case H2_PAL_LOG_WARN:  level_name = "W"; break;
    case H2_PAL_LOG_ERROR: level_name = "E"; break;
    default:
        return H2_PAL_LOG_ERR_INVALID_ARG;
    }

    s_in_write = 1u;
    scope_name = (scope != NULL && scope[0] != '\0') ? scope : "h2";
    scope_length = log_bounded_strlen(scope_name, H2_BK3633_LOG_FORMAT_SIZE);
    message_length = log_bounded_strlen(message, H2_BK3633_LOG_FORMAT_SIZE);
    formatted_length = 8u + scope_length + message_length;
    if (formatted_length >= H2_BK3633_LOG_FORMAT_SIZE) {
        formatted_length = H2_BK3633_LOG_FORMAT_SIZE - 1u;
        line_ending = "\n";
        line_ending_length = 1u;
        result = H2_PAL_LOG_ERR_TRUNCATED;
    } else {
        line_ending = "\r\n";
        line_ending_length = 2u;
    }

    remaining = formatted_length - line_ending_length;
    write_ok &= log_push_segment("[", 1u, &remaining);
    write_ok &= log_push_segment(level_name, 1u, &remaining);
    write_ok &= log_push_segment("][", 2u, &remaining);
    write_ok &= log_push_segment(scope_name, scope_length, &remaining);
    write_ok &= log_push_segment("] ", 2u, &remaining);
    write_ok &= log_push_segment(message, message_length, &remaining);
    if (write_ok != 0) {
        remaining = line_ending_length;
        write_ok &= log_push_segment(line_ending, line_ending_length,
                                     &remaining);
    }
    if (write_ok == 0) {
        result = H2_PAL_LOG_ERR_WRITE;
    }
    s_in_write = 0u;

    return result;
}

const h2_pal_log_api_t *h2_bk3633_platform_log_api(void)
{
    static const h2_pal_log_vtable_t vtable = {
        .write = log_write,
    };
    static const h2_pal_log_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
