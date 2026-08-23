#include "h2_corehttp_internal.h"

#include <limits.h>
#include <string.h>

static bool ascii_name_equal(
    const char *data,
    size_t len,
    const char *literal) {
    size_t literal_len = strlen(literal);
    if (data == NULL || len != literal_len) {
        return false;
    }
    for (size_t index = 0u; index < len; ++index) {
        char value = data[index];
        if (value >= 'A' && value <= 'Z') {
            value = (char)(value - 'A' + 'a');
        }
        if (value != literal[index]) {
            return false;
        }
    }
    return true;
}

static bool retryable_status(int status_code) {
    return status_code == 408 || status_code == 429 || status_code >= 500;
}

static bool redirect_status(int status_code) {
    return status_code == 301 || status_code == 302 || status_code == 303 ||
           status_code == 307 || status_code == 308;
}

static int parser_fail(h2_corehttp_exchange_t *exchange, h2_pal_result_t rc) {
    exchange->result = rc;
    return HPE_USER;
}

static int parser_count_header(
    h2_corehttp_exchange_t *exchange,
    size_t len) {
    if (len > SIZE_MAX - exchange->header_bytes) {
        return parser_fail(exchange, H2_PAL_ERR_NO_SPACE);
    }
    exchange->header_bytes += len;
    if (exchange->header_bytes > exchange->provider->config.max_header_bytes) {
        return parser_fail(exchange, H2_PAL_ERR_NO_SPACE);
    }
    return HPE_OK;
}

static int parser_status(llhttp_t *parser, const char *data, size_t len) {
    (void)data;
    h2_corehttp_exchange_t *exchange =
        (h2_corehttp_exchange_t *)parser->data;
    return parser_count_header(exchange, len);
}

static int ensure_header_capacity(
    h2_corehttp_exchange_t *exchange,
    char **buffer,
    size_t *capacity,
    size_t needed) {
    if (needed > exchange->provider->config.max_header_bytes ||
        needed == SIZE_MAX) {
        return parser_fail(exchange, H2_PAL_ERR_NO_SPACE);
    }
    if (needed <= *capacity) {
        return HPE_OK;
    }
    size_t next_capacity = *capacity == 0u ? 128u : *capacity;
    while (next_capacity < needed) {
        if (next_capacity > SIZE_MAX / 2u) {
            next_capacity = needed;
            break;
        }
        next_capacity *= 2u;
    }
    char *next = (char *)h2_pal_mem_realloc(
        exchange->provider->config.allocator, *buffer, next_capacity + 1u);
    if (next == NULL) {
        return parser_fail(exchange, H2_PAL_ERR_NO_MEMORY);
    }
    *buffer = next;
    *capacity = next_capacity;
    return HPE_OK;
}

static int append_header_span(
    h2_corehttp_exchange_t *exchange,
    char **buffer,
    size_t *length,
    size_t *capacity,
    const char *data,
    size_t len) {
    if (len > SIZE_MAX - *length) {
        return parser_fail(exchange, H2_PAL_ERR_NO_SPACE);
    }
    size_t needed = *length + len;
    int rc = ensure_header_capacity(exchange, buffer, capacity, needed);
    if (rc != HPE_OK) {
        return rc;
    }
    if (len > 0u) {
        memcpy(*buffer + *length, data, len);
    }
    *length = needed;
    if (*buffer != NULL) {
        (*buffer)[needed] = '\0';
    }
    return HPE_OK;
}

static int parser_header_field(
    llhttp_t *parser,
    const char *data,
    size_t len) {
    h2_corehttp_exchange_t *exchange =
        (h2_corehttp_exchange_t *)parser->data;
    int rc = parser_count_header(exchange, len);
    if (rc != HPE_OK) {
        return rc;
    }
    return append_header_span(
        exchange, &exchange->header_name, &exchange->header_name_len,
        &exchange->header_name_cap, data, len);
}

static int parser_header_field_complete(llhttp_t *parser) {
    h2_corehttp_exchange_t *exchange =
        (h2_corehttp_exchange_t *)parser->data;
    exchange->capture_location = ascii_name_equal(
        exchange->header_name, exchange->header_name_len, "location");
    if (exchange->capture_location && exchange->location_seen) {
        return parser_fail(exchange, H2_PAL_ERR_FORMAT);
    }
    return HPE_OK;
}

static int ensure_location_capacity(
    h2_corehttp_exchange_t *exchange,
    size_t needed) {
    if (needed > exchange->provider->config.max_header_bytes) {
        return parser_fail(exchange, H2_PAL_ERR_NO_SPACE);
    }
    if (needed <= exchange->location_cap) {
        return HPE_OK;
    }
    size_t capacity = exchange->location_cap == 0u ? 128u
                                                   : exchange->location_cap;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    char *location = (char *)h2_pal_mem_realloc(
        exchange->provider->config.allocator, exchange->location,
        capacity + 1u);
    if (location == NULL) {
        return parser_fail(exchange, H2_PAL_ERR_NO_MEMORY);
    }
    exchange->location = location;
    exchange->location_cap = capacity;
    return HPE_OK;
}

static int parser_header_value(
    llhttp_t *parser,
    const char *data,
    size_t len) {
    h2_corehttp_exchange_t *exchange =
        (h2_corehttp_exchange_t *)parser->data;
    int rc = parser_count_header(exchange, len);
    if (rc != HPE_OK ||
        (exchange->request->response_header_cb == NULL &&
         !exchange->capture_location)) {
        return rc;
    }
    return append_header_span(
        exchange, &exchange->header_value, &exchange->header_value_len,
        &exchange->header_value_cap, data, len);
}

static int parser_header_value_complete(llhttp_t *parser) {
    h2_corehttp_exchange_t *exchange =
        (h2_corehttp_exchange_t *)parser->data;
    size_t start = 0u;
    while (start < exchange->header_value_len &&
           (exchange->header_value[start] == ' ' ||
            exchange->header_value[start] == '\t')) {
        start += 1u;
    }
    size_t end = exchange->header_value_len;
    while (end > start &&
           (exchange->header_value[end - 1u] == ' ' ||
            exchange->header_value[end - 1u] == '\t')) {
        end -= 1u;
    }
    const char *value = exchange->header_value == NULL
                            ? NULL
                            : exchange->header_value + start;
    size_t value_len = end - start;
    int rc = h2_pal_http_deliver_response_header(
        exchange->request, exchange->header_name,
        exchange->header_name_len, value, value_len);
    if (rc != H2_PAL_OK) {
        return parser_fail(exchange, (h2_pal_result_t)rc);
    }
    if (exchange->capture_location) {
        rc = ensure_location_capacity(exchange, value_len);
        if (rc != HPE_OK) {
            return rc;
        }
        if (value_len > 0u) {
            memcpy(exchange->location, value, value_len);
        }
        exchange->location_len = value_len;
        if (exchange->location != NULL) {
            exchange->location[value_len] = '\0';
        }
        exchange->location_seen = true;
    }
    exchange->header_name_len = 0u;
    exchange->header_value_len = 0u;
    if (exchange->header_name != NULL) {
        exchange->header_name[0] = '\0';
    }
    if (exchange->header_value != NULL) {
        exchange->header_value[0] = '\0';
    }
    exchange->capture_location = false;
    return parser_count_header(exchange, 4u);
}

static int parser_headers_complete(llhttp_t *parser) {
    h2_corehttp_exchange_t *exchange =
        (h2_corehttp_exchange_t *)parser->data;
    exchange->headers_complete = true;
    exchange->response->status_code = llhttp_get_status_code(parser);
    exchange->response->content_length =
        (parser->flags & F_CONTENT_LENGTH) != 0u
            ? (parser->content_length > (uint64_t)INT64_MAX
                   ? INT64_MAX
                   : (int64_t)parser->content_length)
            : -1;
    exchange->suppress_body =
        (exchange->redirect_available &&
         redirect_status(exchange->response->status_code)) ||
        (exchange->retry_available &&
         retryable_status(exchange->response->status_code));
    if (exchange->method == H2_PAL_HTTP_HEAD) {
        return 1;
    }
    return HPE_OK;
}

static int deliver_body(
    h2_corehttp_exchange_t *exchange,
    const uint8_t *data,
    size_t len) {
    if (exchange->suppress_body || len == 0u) {
        return H2_PAL_OK;
    }
    const h2_pal_http_request_t *request = exchange->request;
    h2_pal_http_response_t *response = exchange->response;
    if (request->read_cb != NULL) {
        size_t offset = 0u;
        while (offset < len) {
            if (h2_pal_http_request_is_canceled(request)) {
                return H2_PAL_ERR_CLOSED;
            }
            size_t chunk_len = len - offset;
            const uint8_t *chunk = data + offset;
            if (request->chunk_buf != NULL && request->chunk_buf_cap > 0u) {
                if (chunk_len > request->chunk_buf_cap) {
                    chunk_len = request->chunk_buf_cap;
                }
                memcpy(request->chunk_buf, chunk, chunk_len);
                chunk = request->chunk_buf;
            }
            if (chunk_len > SIZE_MAX - exchange->total_read) {
                return H2_PAL_ERR_NO_SPACE;
            }
            size_t total = exchange->total_read + chunk_len;
            size_t remaining = 0u;
            if (response->content_length >= 0 &&
                (uint64_t)response->content_length > (uint64_t)total) {
                uint64_t difference =
                    (uint64_t)response->content_length - (uint64_t)total;
                remaining = difference > SIZE_MAX ? SIZE_MAX
                                                  : (size_t)difference;
            }
            int rc = request->read_cb(request->user, request, chunk, chunk_len,
                                      total, remaining);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            exchange->total_read = total;
            response->body_len = total;
            exchange->body_delivered = true;
            offset += chunk_len;
        }
        return H2_PAL_OK;
    }

    if (request->response_buf != NULL) {
        if (response->body_len > request->response_buf_cap ||
            len > request->response_buf_cap - response->body_len) {
            return H2_PAL_ERR_NO_SPACE;
        }
        memcpy(request->response_buf + response->body_len, data, len);
        response->body = request->response_buf;
        response->body_len += len;
        response->allocator = NULL;
        exchange->total_read = response->body_len;
        return H2_PAL_OK;
    }

    const h2_pal_mem_api_t *allocator =
        h2_pal_http_response_allocator(request);
    if (allocator != NULL) {
        if (len > SIZE_MAX - response->body_len) {
            return H2_PAL_ERR_NO_SPACE;
        }
        size_t needed = response->body_len + len;
        uint8_t *body = (uint8_t *)h2_pal_mem_realloc(
            allocator, response->body, needed);
        if (body == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(body + response->body_len, data, len);
        response->body = body;
        response->body_len = needed;
        response->allocator = allocator;
        exchange->total_read = needed;
        return H2_PAL_OK;
    }

    if (len > SIZE_MAX - exchange->total_read) {
        return H2_PAL_ERR_NO_SPACE;
    }
    exchange->total_read += len;
    response->body_len = exchange->total_read;
    return H2_PAL_OK;
}

static int parser_body(llhttp_t *parser, const char *data, size_t len) {
    h2_corehttp_exchange_t *exchange =
        (h2_corehttp_exchange_t *)parser->data;
    h2_pal_result_t rc = deliver_body(
        exchange, (const uint8_t *)data, len);
    return rc == H2_PAL_OK ? HPE_OK : parser_fail(exchange, rc);
}

static int parser_message_complete(llhttp_t *parser) {
    h2_corehttp_exchange_t *exchange =
        (h2_corehttp_exchange_t *)parser->data;
    if (exchange->response->status_code >= 100 &&
        exchange->response->status_code < 200 &&
        exchange->response->status_code != 101) {
        exchange->response->status_code = 0;
        exchange->response->content_length = -1;
        exchange->headers_complete = false;
        exchange->location_len = 0u;
        exchange->location_seen = false;
        exchange->capture_location = false;
        exchange->header_name_len = 0u;
        exchange->header_value_len = 0u;
        return HPE_OK;
    }
    exchange->message_complete = true;
    return HPE_PAUSED;
}

static h2_pal_result_t finish_parser(h2_corehttp_exchange_t *exchange) {
    llhttp_errno_t parser_rc = llhttp_finish(&exchange->parser);
    if ((parser_rc == HPE_OK || parser_rc == HPE_PAUSED) &&
        exchange->message_complete) {
        return H2_PAL_OK;
    }
    if (exchange->result != H2_PAL_OK) {
        return exchange->result;
    }
    return H2_PAL_ERR_FORMAT;
}

h2_pal_result_t h2_corehttp_receive_response(
    h2_corehttp_exchange_t *exchange) {
    if (exchange == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    llhttp_settings_init(&exchange->parser_settings);
    exchange->parser_settings.on_status = parser_status;
    exchange->parser_settings.on_header_field = parser_header_field;
    exchange->parser_settings.on_header_field_complete =
        parser_header_field_complete;
    exchange->parser_settings.on_header_value = parser_header_value;
    exchange->parser_settings.on_header_value_complete =
        parser_header_value_complete;
    exchange->parser_settings.on_headers_complete = parser_headers_complete;
    exchange->parser_settings.on_body = parser_body;
    exchange->parser_settings.on_message_complete = parser_message_complete;
    llhttp_init(&exchange->parser, HTTP_RESPONSE, &exchange->parser_settings);
    exchange->parser.data = exchange;

    uint8_t buffer[H2_COREHTTP_RECV_BYTES];
    for (;;) {
        int received = h2_corehttp_transport_recv(
            &exchange->network, buffer, sizeof(buffer));
        if (received == 0) {
            continue;
        }
        if (received < 0) {
            if (exchange->result == H2_PAL_ERR_CLOSED) {
                return finish_parser(exchange);
            }
            return exchange->result == H2_PAL_OK ? H2_PAL_ERR_IO
                                                 : exchange->result;
        }
        llhttp_errno_t parser_rc = llhttp_execute(
            &exchange->parser, (const char *)buffer, (size_t)received);
        if (parser_rc == HPE_PAUSED && exchange->message_complete) {
            const char *position = llhttp_get_error_pos(&exchange->parser);
            if (position != NULL &&
                position < (const char *)buffer + received) {
                return H2_PAL_ERR_FORMAT;
            }
            return H2_PAL_OK;
        }
        if (parser_rc != HPE_OK) {
            return exchange->result == H2_PAL_OK ? H2_PAL_ERR_FORMAT
                                                 : exchange->result;
        }
    }
}
