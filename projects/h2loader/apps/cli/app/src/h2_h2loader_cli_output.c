#include "h2_h2loader_cli_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const h2_command_io_api_t *stream_io(
    const h2_h2loader_cli_context_t *context,
    h2_h2loader_cli_stream_t stream) {
    if (context == NULL || context->config == NULL) return NULL;
    if (stream != H2_H2LOADER_CLI_STREAM_STDOUT &&
        stream != H2_H2LOADER_CLI_STREAM_STDERR) {
        return NULL;
    }
    return stream == H2_H2LOADER_CLI_STREAM_STDOUT
        ? context->config->stdout_io
        : context->config->stderr_io;
}

static h2_pal_result_t write_all(
    const h2_command_io_api_t *io,
    const uint8_t *data,
    size_t len) {
    if (io == NULL || io->vtable == NULL || io->vtable->write == NULL ||
        (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    while (len != 0u) {
        size_t written = 0u;
        h2_pal_result_t result = io->vtable->write(
            io->user, data, len, &written, UINT32_MAX);
        if (result != H2_PAL_OK) return result;
        if (written == 0u || written > len) return H2_PAL_ERR_IO;
        data += written;
        len -= written;
    }
    return io->vtable->flush == NULL
        ? H2_PAL_OK
        : io->vtable->flush(io->user);
}

h2_pal_result_t h2_h2loader_cli_output_bytes(
    h2_h2loader_cli_context_t *context,
    h2_h2loader_cli_stream_t stream,
    const uint8_t *data,
    size_t len) {
    return write_all(stream_io(context, stream), data, len);
}

h2_pal_result_t h2_h2loader_cli_output(
    h2_h2loader_cli_context_t *context,
    h2_h2loader_cli_stream_t stream,
    const char *format,
    ...) {
    char buffer[4096];
    va_list args;
    int len;

    const h2_command_io_api_t *io = stream_io(context, stream);
    if (io == NULL || format == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    va_start(args, format);
    len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (len < 0 || (size_t)len >= sizeof(buffer)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return write_all(io, (const uint8_t *)buffer, (size_t)len);
}

h2_pal_result_t h2_h2loader_cli_output_json_string(
    h2_h2loader_cli_context_t *context,
    h2_h2loader_cli_stream_t stream,
    const char *value) {
    static const char hex[] = "0123456789abcdef";
    char escaped[4096];
    size_t out = 0u;
    const unsigned char *cursor = (const unsigned char *)value;
    if (value == NULL) return H2_PAL_ERR_INVALID_ARG;
    escaped[out++] = '"';
    while (*cursor != 0u) {
        if (*cursor == '"' || *cursor == '\\') {
            if (out + 2u >= sizeof(escaped)) return H2_PAL_ERR_INVALID_ARG;
            escaped[out++] = '\\';
            escaped[out++] = (char)*cursor;
        } else if (*cursor < 0x20u) {
            if (out + 6u >= sizeof(escaped)) return H2_PAL_ERR_INVALID_ARG;
            escaped[out++] = '\\';
            escaped[out++] = 'u';
            escaped[out++] = '0';
            escaped[out++] = '0';
            escaped[out++] = hex[*cursor >> 4u];
            escaped[out++] = hex[*cursor & 0x0fu];
        } else {
            if (out + 1u >= sizeof(escaped)) return H2_PAL_ERR_INVALID_ARG;
            escaped[out++] = (char)*cursor;
        }
        ++cursor;
    }
    if (out + 1u >= sizeof(escaped)) return H2_PAL_ERR_INVALID_ARG;
    escaped[out++] = '"';
    return write_all(stream_io(context, stream), (const uint8_t *)escaped, out);
}

h2_pal_result_t h2_h2loader_cli_transport_log(
    void *user,
    const uint8_t *data,
    size_t len) {
    h2_h2loader_cli_context_t *context = user;
    if (context == NULL || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t index = 0u; index < len; ++index) {
        uint8_t value = data[index];
        if (value == 0u || (value < 0x20u && value != '\r' && value != '\n' &&
                            value != '\t') || value >= 0x7fu) {
            context->transport_log_line_discard = 1u;
        }
        if (context->transport_log_line_len <
            sizeof(context->transport_log_line)) {
            context->transport_log_line[context->transport_log_line_len++] =
                value;
        } else {
            context->transport_log_line_discard = 1u;
        }
        if (value == '\n') {
            h2_pal_result_t rc = H2_PAL_OK;
            if (!context->transport_log_line_discard) {
                rc = h2_h2loader_cli_output_bytes(
                    context,
                    H2_H2LOADER_CLI_STREAM_STDOUT,
                    context->transport_log_line,
                    context->transport_log_line_len);
            }
            context->transport_log_line_len = 0u;
            context->transport_log_line_discard = 0u;
            if (rc != H2_PAL_OK) return rc;
        }
    }
    return H2_PAL_OK;
}
