#include "h2_windows_internal.h"

#include <stdio.h>
#include <string.h>

static int windows_log_write(
    void *user,
    h2_pal_log_level_t level,
    const char *scope,
    const char *message) {
    (void)user;
    static const char *const levels[] = {"debug", "info", "warn", "error"};
    const char *level_name = level >= H2_PAL_LOG_DEBUG &&
                                     level <= H2_PAL_LOG_ERROR
                                 ? levels[level]
                                 : "unknown";
    const char *scope_value = scope != NULL ? scope : "h2";
    const size_t scope_limit = 64u;
    size_t scope_len = strnlen_s(scope_value, scope_limit + 1u);
    size_t message_len = strnlen_s(message, H2_PAL_LOG_MESSAGE_MAX + 1u);
    char record[H2_PAL_LOG_MESSAGE_MAX + 96u];
    int written = snprintf(record, sizeof(record),
                           "H2_LOG level=%s scope=%.*s message=%.*s\n",
                           level_name, (int)scope_limit, scope_value,
                           (int)H2_PAL_LOG_MESSAGE_MAX, message);
    if (written < 0) {
        return H2_PAL_ERR_FORMAT;
    }
    record[sizeof(record) - 1u] = '\0';
    if (fputs(record, stderr) == EOF) {
        return H2_PAL_ERR_WRITE;
    }
    wchar_t *wide = h2_windows_utf8_to_wide(record);
    if (wide != NULL) {
        OutputDebugStringW(wide);
        h2_windows_heap_free(wide);
    }
    return scope_len > scope_limit ||
                   message_len > H2_PAL_LOG_MESSAGE_MAX ||
                   (size_t)written >= sizeof(record)
               ? H2_PAL_ERR_TRUNCATED
               : H2_PAL_OK;
}

const h2_pal_log_vtable_t h2_windows_log_vtable = {
    .write = windows_log_write,
};
