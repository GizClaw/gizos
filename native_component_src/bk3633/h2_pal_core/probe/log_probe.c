#include "h2_bk3633_platform_core.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct probe_sink {
    uint8_t data[512];
    size_t length;
    size_t largest_write;
} probe_sink_t;

static size_t probe_write(void *user, const uint8_t *data, size_t length)
{
    probe_sink_t *sink = user;

    if (sink == NULL || data == NULL ||
        length > sizeof(sink->data) - sink->length) {
        return 0u;
    }
    memcpy(&sink->data[sink->length], data, length);
    sink->length += length;
    if (length > sink->largest_write) {
        sink->largest_write = length;
    }
    return length;
}

int main(void)
{
    static const char expected[] = "[I][probe] bounded\r\n";
    probe_sink_t sink = {0};
    const h2_bk3633_platform_log_config_t config = {
        .write = probe_write,
        .user = &sink,
        .max_write_size = 7u,
    };
    const h2_pal_log_api_t *log = h2_bk3633_platform_log_api();

    if (h2_bk3633_platform_log_init(&config) != H2_PAL_OK) {
        return 1;
    }
    if (h2_pal_log_write(log, H2_PAL_LOG_INFO, "probe", "bounded") !=
            H2_PAL_OK ||
        sink.length != 0u) {
        return 2;
    }
    for (size_t i = 0u; i < 16u && sink.length < sizeof(expected) - 1u; ++i) {
        if (h2_bk3633_platform_log_drain() != H2_PAL_OK) {
            return 3;
        }
    }
    if (sink.length != sizeof(expected) - 1u ||
        memcmp(sink.data, expected, sizeof(expected) - 1u) != 0 ||
        sink.largest_write > config.max_write_size) {
        return 4;
    }

    char long_message[400];
    memset(long_message, 'x', sizeof(long_message) - 1u);
    long_message[sizeof(long_message) - 1u] = '\0';
    if (h2_pal_log_write(log, H2_PAL_LOG_WARN, "scope", long_message) !=
        H2_PAL_LOG_ERR_TRUNCATED) {
        return 6;
    }
    const size_t truncated_offset = sizeof(expected) - 1u;
    const size_t truncated_length = 319u;
    for (size_t i = 0u;
         i < 64u && sink.length < truncated_offset + truncated_length;
         ++i) {
        if (h2_bk3633_platform_log_drain() != H2_PAL_OK) {
            return 7;
        }
    }
    static const char truncated_prefix[] = "[W][scope] ";
    if (sink.length != truncated_offset + truncated_length ||
        memcmp(&sink.data[truncated_offset], truncated_prefix,
               sizeof(truncated_prefix) - 1u) != 0 ||
        sink.data[sink.length - 1u] != (uint8_t)'\n') {
        return 8;
    }
    for (size_t i = truncated_offset + sizeof(truncated_prefix) - 1u;
         i + 1u < sink.length; ++i) {
        if (sink.data[i] != (uint8_t)'x') {
            return 9;
        }
    }
    h2_bk3633_platform_log_deinit();
    if (h2_bk3633_platform_log_drain() != H2_PAL_ERR_INVALID_STATE) {
        return 5;
    }
    return 0;
}
