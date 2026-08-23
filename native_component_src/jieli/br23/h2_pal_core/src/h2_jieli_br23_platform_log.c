#include "h2_jieli_br23_platform_core.h"
#include "h2_jieli_br23_sdk_port.h"

#include <string.h>

static char level_tag(h2_pal_log_level_t level)
{
    switch (level) {
    case H2_PAL_LOG_DEBUG:
        return 'D';
    case H2_PAL_LOG_INFO:
        return 'I';
    case H2_PAL_LOG_WARN:
        return 'W';
    case H2_PAL_LOG_ERROR:
        return 'E';
    default:
        return '?';
    }
}

static size_t append(char *line, size_t capacity, size_t length, const char *text)
{
    size_t index = 0u;
    if (text == NULL) {
        return length;
    }
    while (text[index] != '\0' && length + 1u < capacity) {
        line[length++] = text[index++];
    }
    return length;
}

static int log_write(void *user, h2_pal_log_level_t level, const char *scope, const char *message)
{
    /* The Log contract allows H2_PAL_LOG_MESSAGE_MAX bytes; the prefix and
     * CRLF fit inside H2_JIELI_BR23_LOG_LINE_MAX without truncating it. */
    char line[H2_JIELI_BR23_LOG_LINE_MAX];
    size_t length = 0u;
    (void)user;
    if (message == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    /* Reserve room for the fixed delimiters that follow each variable part:
     * after the scope come "] " (2 bytes) plus CRLF (2 bytes); after the
     * message only CRLF (2 bytes). append() never writes past `capacity`. */
    line[length++] = '[';
    line[length++] = level_tag(level);
    line[length++] = ']';
    if (scope != NULL && scope[0] != '\0') {
        line[length++] = '[';
        length = append(line, sizeof(line) - 4u, length, scope);
        line[length++] = ']';
    }
    line[length++] = ' ';
    length = append(line, sizeof(line) - 2u, length, message);
    line[length++] = '\r';
    line[length++] = '\n';
    h2_jieli_sdk_debug_write(line, length);
    return H2_PAL_OK;
}

static const h2_pal_log_vtable_t s_log_vtable = {
    .write = log_write,
};

static const h2_pal_log_api_t s_log_api = {
    .user = NULL,
    .vtable = &s_log_vtable,
};

const h2_pal_log_api_t *h2_jieli_br23_platform_log_api(void)
{
    return &s_log_api;
}
