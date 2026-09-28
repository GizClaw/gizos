#ifndef H2_PAL_HTTP_H
#define H2_PAL_HTTP_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/core/h2_pal_types.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_http_method {
    H2_PAL_HTTP_GET = 1,
    H2_PAL_HTTP_POST = 2,
    H2_PAL_HTTP_PUT = 3,
    H2_PAL_HTTP_PATCH = 4,
    H2_PAL_HTTP_DELETE = 5,
    H2_PAL_HTTP_HEAD = 6,
    H2_PAL_HTTP_OPTIONS = 7,
} h2_pal_http_method_t;

#define H2_PAL_HTTP_METHOD_GET H2_PAL_HTTP_GET
#define H2_PAL_HTTP_METHOD_POST H2_PAL_HTTP_POST
#define H2_PAL_HTTP_METHOD_PUT H2_PAL_HTTP_PUT
#define H2_PAL_HTTP_METHOD_PATCH H2_PAL_HTTP_PATCH
#define H2_PAL_HTTP_METHOD_DELETE H2_PAL_HTTP_DELETE
#define H2_PAL_HTTP_METHOD_HEAD H2_PAL_HTTP_HEAD
#define H2_PAL_HTTP_METHOD_OPTIONS H2_PAL_HTTP_OPTIONS

typedef struct h2_pal_http_str {
    const char *data;
    size_t len;
} h2_pal_http_str_t;

typedef struct h2_pal_http_bytes {
    const uint8_t *data;
    size_t len;
} h2_pal_http_bytes_t;

typedef struct h2_pal_http_header {
    h2_pal_http_str_t name;
    h2_pal_http_str_t value;
} h2_pal_http_header_t;

typedef struct h2_pal_http_request h2_pal_http_request_t;

/** Return non-zero to cancel an in-flight request. */
typedef int (*h2_pal_http_cancel_fn)(void *user);

typedef int (*h2_pal_http_read_fn)(
    void *user,
    const h2_pal_http_request_t *request,
    const uint8_t *chunk,
    size_t chunk_len,
    size_t total_read,
    size_t remaining);

/**
 * @brief Receive one response header during a synchronous request.
 *
 * Header name and value are borrowed, non-NUL-dependent byte spans valid only
 * for the callback. A non-`H2_PAL_OK` result aborts the request and is returned
 * to the caller. Backends may invoke the callback again for each retry attempt;
 * they never retain the callback or its context after `request()` returns.
 */
typedef int (*h2_pal_http_response_header_fn)(
    void *user,
    const h2_pal_http_request_t *request,
    h2_pal_http_str_t name,
    h2_pal_http_str_t value);

typedef struct h2_pal_http_request {
    h2_pal_http_method_t method;
    h2_pal_http_str_t url;
    const h2_pal_http_header_t *headers;
    size_t header_count;
    const uint8_t *body;
    size_t body_len;

    h2_pal_http_response_header_fn response_header_cb;
    void *response_header_user;

    const char *interface_name;
    int timeout_ms;
    int retry_count;

    uint8_t *chunk_buf;
    size_t chunk_buf_cap;

    uint8_t *response_buf;
    size_t response_buf_cap;
    const h2_pal_mem_api_t *response_allocator;

    h2_pal_http_read_fn read_cb;
    void *user;

    h2_pal_http_cancel_fn cancel_cb;
    void *cancel_user;

    /*
     * Backward-compatible allocator alias. New code should use
     * response_allocator for response ownership. Implementations may also use
     * this allocator for short-lived request scratch allocations.
     */
    const h2_pal_mem_api_t *allocator;
} h2_pal_http_request_t;

static inline int h2_pal_http_request_is_canceled(
    const h2_pal_http_request_t *request) {
    return request != NULL && request->cancel_cb != NULL &&
        request->cancel_cb(request->cancel_user) != 0;
}

static inline int h2_pal_http_deliver_response_header(
    const h2_pal_http_request_t *request,
    const char *name,
    size_t name_len,
    const char *value,
    size_t value_len) {
    if (request == NULL || name == NULL || name_len == 0u ||
        (value == NULL && value_len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (memchr(name, '\0', name_len) != NULL ||
        memchr(name, '\r', name_len) != NULL ||
        memchr(name, '\n', name_len) != NULL ||
        (value_len != 0u &&
         (memchr(value, '\0', value_len) != NULL ||
          memchr(value, '\r', value_len) != NULL ||
          memchr(value, '\n', value_len) != NULL))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (request->response_header_cb == NULL) {
        return H2_PAL_OK;
    }
    const h2_pal_http_str_t header_name = {name, name_len};
    const h2_pal_http_str_t header_value = {value, value_len};
    return request->response_header_cb(
        request->response_header_user, request, header_name, header_value);
}

typedef struct h2_pal_http_response {
    int status_code;
    int64_t content_length;
    uint8_t *body;
    size_t body_len;
    const h2_pal_mem_api_t *allocator;
} h2_pal_http_response_t;

typedef struct h2_pal_http_vtable {
    int (*request)(void *user, const h2_pal_http_request_t *request, h2_pal_http_response_t *out_response);
    void (*response_free)(void *user, h2_pal_http_response_t *response);
} h2_pal_http_vtable_t;

typedef struct h2_pal_http_api {
    void *user;
    const h2_pal_http_vtable_t *vtable;
} h2_pal_http_api_t;

static inline void h2_pal_http_response_reset(h2_pal_http_response_t *response) {
    if (response == NULL) {
        return;
    }
    response->status_code = 0;
    response->content_length = 0;
    response->body = NULL;
    response->body_len = 0;
    response->allocator = NULL;
}

static inline int h2_pal_http_status_has_error(int status_code) {
    return status_code < 200 || status_code >= 400;
}

static inline const h2_pal_mem_api_t *h2_pal_http_response_allocator(
    const h2_pal_http_request_t *request) {
    if (request == NULL) {
        return NULL;
    }
    if (request->response_allocator != NULL) {
        return request->response_allocator;
    }
    return request->allocator;
}

static inline int h2_pal_http_request_uses_caller_buffer(
    const h2_pal_http_request_t *request) {
    return request != NULL && request->read_cb == NULL && request->response_buf != NULL;
}

static inline int h2_pal_http_request_uses_allocator(
    const h2_pal_http_request_t *request) {
    return request != NULL &&
        request->read_cb == NULL &&
        request->response_buf == NULL &&
        h2_pal_http_response_allocator(request) != NULL;
}

static inline int h2_pal_http_do(
    const h2_pal_http_api_t *api,
    const h2_pal_http_request_t *request,
    h2_pal_http_response_t *out_response) {
    if (api == NULL ||
        api->vtable == NULL ||
        api->vtable->request == NULL ||
        request == NULL ||
        out_response == NULL ||
        request->url.data == NULL ||
        request->url.len == 0u ||
        (request->header_count > 0u && request->headers == NULL) ||
        (request->body_len > 0u && request->body == NULL) ||
        (request->chunk_buf_cap > 0u && request->chunk_buf == NULL) ||
        (request->response_buf_cap > 0u && request->response_buf == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->request(api->user, request, out_response);
}

static inline int h2_pal_http_request(
    const h2_pal_http_api_t *api,
    const h2_pal_http_request_t *request,
    h2_pal_http_response_t *out_response) {
    return h2_pal_http_do(api, request, out_response);
}

static inline void h2_pal_http_response_free(
    const h2_pal_http_api_t *api,
    h2_pal_http_response_t *response) {
    if (response == NULL) {
        return;
    }
    if (api != NULL && api->vtable != NULL && api->vtable->response_free != NULL) {
        api->vtable->response_free(api->user, response);
        return;
    }
    if (response->allocator != NULL && response->body != NULL) {
        h2_pal_mem_free(response->allocator, response->body);
    }
    h2_pal_http_response_reset(response);
}

#ifdef __cplusplus
}
#endif

#endif
