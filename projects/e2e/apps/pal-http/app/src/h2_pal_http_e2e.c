#include "h2_pal_http_e2e.h"

#include <stdio.h>
#include <string.h>

#define HTTP_BODY_BYTES 513u
#define HTTP_URL_BYTES 512u
#define HTTP_ALLOCATION_SLOTS 32u
#define CHECK(value) do { if (!(value)) { result->line = __LINE__; \
    result->detail = rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc; goto done; } } while (0)

static const char *const case_ids[] = {
#define H2_PAL_HTTP_CASE(symbol, id) id,
#include "h2_pal_http_cases.inc"
#undef H2_PAL_HTTP_CASE
};

typedef struct allocation_tracker {
    const h2_pal_mem_api_t *backing;
    void *slots[HTTP_ALLOCATION_SLOTS];
    size_t live;
    size_t allocations;
    int fail;
    int invalid_free;
} allocation_tracker_t;

static void *tracked_realloc(void *user, void *ptr, size_t len) {
    allocation_tracker_t *tracker = user;
    size_t slot = HTTP_ALLOCATION_SLOTS;
    for (size_t index = 0u; index < HTTP_ALLOCATION_SLOTS; ++index) {
        if (tracker->slots[index] == ptr) { slot = index; break; }
    }
    if (slot == HTTP_ALLOCATION_SLOTS) {
        tracker->invalid_free = 1;
        return NULL;
    }
    ++tracker->allocations;
    if (tracker->fail) return NULL;
    if (len == 0u) len = 1u;
    void *next = h2_pal_mem_realloc(tracker->backing, ptr, len);
    if (next != NULL) {
        if (ptr == NULL) ++tracker->live;
        tracker->slots[slot] = next;
    }
    return next;
}

static void *tracked_alloc(void *user, size_t len) {
    return tracked_realloc(user, NULL, len);
}

static void tracked_free(void *user, void *ptr) {
    allocation_tracker_t *tracker = user;
    if (ptr == NULL) return;
    for (size_t index = 0u; index < HTTP_ALLOCATION_SLOTS; ++index) {
        if (tracker->slots[index] == ptr) {
            tracker->slots[index] = NULL;
            --tracker->live;
            h2_pal_mem_free(tracker->backing, ptr);
            return;
        }
    }
    tracker->invalid_free = 1;
}

static const h2_pal_mem_vtable_t tracked_vtable = {
    .alloc = tracked_alloc, .realloc = tracked_realloc, .free = tracked_free,
};

typedef struct callback_state {
    const h2_pal_time_api_t *time;
    const h2_pal_http_request_t *request;
    size_t received;
    unsigned reads;
    unsigned headers;
    unsigned method_headers;
    unsigned marker_headers;
    int header_abort;
    int read_abort;
    int cancel;
    int cancel_after_read;
    int invalid;
    int unknown_length;
    uint64_t cancel_at_ms;
    const char *method;
    const uint8_t *chunk_buf;
    size_t chunk_cap;
    unsigned attempt;
} callback_state_t;

static uint8_t payload_byte(size_t offset) {
    return (uint8_t)((offset * 37u + 11u) % 251u);
}

static int span_equal(h2_pal_http_str_t value, const char *literal) {
    return value.len == strlen(literal) &&
        (value.len == 0u || memcmp(value.data, literal, value.len) == 0);
}

static int name_equal(h2_pal_http_str_t value, const char *literal) {
    if (value.len != strlen(literal)) return 0;
    for (size_t index = 0u; index < value.len; ++index) {
        char letter = value.data[index];
        if (letter >= 'A' && letter <= 'Z') letter += 'a' - 'A';
        if (letter != literal[index]) return 0;
    }
    return 1;
}

static int header_callback(void *user, const h2_pal_http_request_t *request,
                           h2_pal_http_str_t name, h2_pal_http_str_t value) {
    callback_state_t *state = user;
    ++state->headers;
    if (request != state->request || name.data == NULL || name.len == 0u ||
        (value.data == NULL && value.len != 0u)) state->invalid = 1;
    if (name_equal(name, "x-h2-method")) {
        ++state->method_headers;
        if (state->method != NULL && !span_equal(value, state->method))
            state->invalid = 1;
    }
    if (name_equal(name, "x-h2-marker")) {
        ++state->marker_headers;
        if (!span_equal(value, "byte-span")) state->invalid = 1;
    }
    if (name_equal(name, "x-h2-attempt") && value.len == 1u)
        state->attempt = (unsigned)(value.data[0] - '0');
    return state->header_abort ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static int read_callback(void *user, const h2_pal_http_request_t *request,
                         const uint8_t *chunk, size_t len, size_t total,
                         size_t remaining) {
    callback_state_t *state = user;
    ++state->reads;
    if (request != state->request || chunk == NULL || len == 0u ||
        total != state->received + len || total > HTTP_BODY_BYTES ||
        (state->chunk_buf != NULL &&
         (chunk != state->chunk_buf || len > state->chunk_cap)) ||
        remaining != (state->unknown_length ? 0u : HTTP_BODY_BYTES - total))
        state->invalid = 1;
    for (size_t index = 0u; index < len; ++index)
        if (chunk[index] != payload_byte(state->received + index))
            state->invalid = 1;
    state->received += len;
    if (state->cancel_after_read) state->cancel = 1;
    return state->read_abort ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static int cancel_callback(void *user) {
    callback_state_t *state = user;
    if (state->cancel_at_ms != 0u) {
        uint64_t now = 0u;
        if (h2_pal_time_get_monotonic_ms(state->time, &now) != H2_PAL_OK ||
            now >= state->cancel_at_ms) state->cancel = 1;
    }
    return state->cancel;
}

static int body_matches(const h2_pal_http_response_t *response) {
    if (response->body == NULL || response->body_len != HTTP_BODY_BYTES)
        return 0;
    for (size_t index = 0u; index < response->body_len; ++index)
        if (response->body[index] != payload_byte(index)) return 0;
    return 1;
}

static int response_empty(const h2_pal_http_response_t *response) {
    return response->body == NULL && response->body_len == 0u &&
        response->allocator == NULL && response->status_code == 0 &&
        response->content_length == 0;
}

static void execute_case(const h2_pal_http_e2e_config_t *config,
                         h2_pal_http_e2e_case_t id,
                         h2_pal_http_e2e_case_result_t *result,
                         size_t *retained) {
    const h2_runtime_t *runtime = config->runtime;
    const h2_pal_http_api_t *http = runtime->http;
    allocation_tracker_t tracked = {.backing = runtime->mem};
    allocation_tracker_t alias = {.backing = runtime->mem};
    h2_pal_mem_api_t allocator = {.user = &tracked, .vtable = &tracked_vtable};
    h2_pal_mem_api_t alias_allocator = {.user = &alias, .vtable = &tracked_vtable};
    uint8_t output[HTTP_BODY_BYTES + 2u];
    uint8_t upload[HTTP_BODY_BYTES];
    uint8_t chunk[23];
    char url[HTTP_URL_BYTES];
    const char *base = config->http_base;
    const char *path = "/bytes";
    callback_state_t callbacks = {.time = runtime->time, .method = "GET"};
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .response_buf = output + 1u,
        .response_buf_cap = HTTP_BODY_BYTES,
        .timeout_ms = 10000,
        .response_header_cb = header_callback,
        .response_header_user = &callbacks,
        .cancel_cb = cancel_callback,
        .cancel_user = &callbacks,
    };
    callbacks.request = &request;
    h2_pal_http_response_t response = {0};
    int rc = H2_PAL_OK;
    int expected_rc = H2_PAL_OK;
    int expected_status = 200;
    int check_body = 1;
    uint64_t started = 0u;
    uint64_t finished = 0u;
    memset(output, 0xa5, sizeof(output));
    for (size_t index = 0u; index < sizeof(upload); ++index)
        upload[index] = payload_byte(index);
    h2_pal_http_header_t header = {0};
    /* Trailing poison is deliberately outside each valid borrowed span. */
    const char header_name[] = "X-H2-Input!poison";
    const char header_value[] = "byte-span!poison";
    const char invalid_name[] = {'x', '\r', '\n', 'y'};
    switch (id) {
        case H2_PAL_HTTP_E2E_API_COMPLETE:
            result->passed = 1;
            goto done;
        case H2_PAL_HTTP_E2E_GET: break;
        case H2_PAL_HTTP_E2E_POST:
        case H2_PAL_HTTP_E2E_PUT:
        case H2_PAL_HTTP_E2E_PATCH:
        case H2_PAL_HTTP_E2E_DELETE:
        case H2_PAL_HTTP_E2E_OPTIONS:
            if (id == H2_PAL_HTTP_E2E_POST) { request.method = H2_PAL_HTTP_POST; callbacks.method = "POST"; }
            if (id == H2_PAL_HTTP_E2E_PUT) { request.method = H2_PAL_HTTP_PUT; callbacks.method = "PUT"; }
            if (id == H2_PAL_HTTP_E2E_PATCH) { request.method = H2_PAL_HTTP_PATCH; callbacks.method = "PATCH"; }
            if (id == H2_PAL_HTTP_E2E_DELETE) { request.method = H2_PAL_HTTP_DELETE; callbacks.method = "DELETE"; }
            if (id == H2_PAL_HTTP_E2E_OPTIONS) { request.method = H2_PAL_HTTP_OPTIONS; callbacks.method = "OPTIONS"; }
            request.body = upload;
            request.body_len = sizeof(upload);
            path = "/echo";
            break;
        case H2_PAL_HTTP_E2E_HEAD:
            request.method = H2_PAL_HTTP_HEAD;
            callbacks.method = "HEAD";
            check_body = 0;
            break;
        case H2_PAL_HTTP_E2E_BYTE_SPANS:
            header.name = (h2_pal_http_str_t){header_name, 10u};
            header.value = (h2_pal_http_str_t){header_value, 9u};
            request.headers = &header;
            request.header_count = 1u;
            path = "/headers";
            break;
        case H2_PAL_HTTP_E2E_EMPTY_BODY:
            path = "/empty"; check_body = 0; expected_status = 204; break;
        case H2_PAL_HTTP_E2E_STATUS_404:
            path = "/status/404"; expected_status = 404; break;
        case H2_PAL_HTTP_E2E_STATUS_503:
            path = "/status/503"; expected_status = 503; break;
        case H2_PAL_HTTP_E2E_CALLER_EXACT: break;
        case H2_PAL_HTTP_E2E_CALLER_TOO_SMALL:
            request.response_buf_cap = HTTP_BODY_BYTES - 1u;
            expected_rc = H2_PAL_ERR_NO_SPACE;
            break;
        case H2_PAL_HTTP_E2E_ALLOCATOR_OWNED:
        case H2_PAL_HTTP_E2E_ALLOCATOR_ALIAS:
        case H2_PAL_HTTP_E2E_ALLOCATOR_PRECEDENCE:
        case H2_PAL_HTTP_E2E_ALLOCATOR_FAILURE:
            request.response_buf = NULL;
            request.response_buf_cap = 0u;
            request.response_allocator = &allocator;
            if (id == H2_PAL_HTTP_E2E_ALLOCATOR_ALIAS) {
                request.response_allocator = NULL;
                request.allocator = &allocator;
            }
            if (id == H2_PAL_HTTP_E2E_ALLOCATOR_PRECEDENCE)
                request.allocator = &alias_allocator;
            if (id == H2_PAL_HTTP_E2E_ALLOCATOR_FAILURE) {
                tracked.fail = 1; expected_rc = H2_PAL_ERR_NO_MEMORY;
            }
            break;
        case H2_PAL_HTTP_E2E_STREAM_KNOWN:
        case H2_PAL_HTTP_E2E_STREAM_CHUNKED:
        case H2_PAL_HTTP_E2E_STREAM_NO_SCRATCH:
        case H2_PAL_HTTP_E2E_STREAM_ABORT:
        case H2_PAL_HTTP_E2E_CANCEL_STREAM:
        case H2_PAL_HTTP_E2E_CALLBACK_QUIESCENCE:
            request.read_cb = read_callback;
            request.user = &callbacks;
            request.chunk_buf = chunk;
            request.chunk_buf_cap = sizeof(chunk);
            callbacks.chunk_buf = chunk;
            callbacks.chunk_cap = sizeof(chunk);
            /* These must remain untouched because streaming has precedence. */
            request.response_allocator = &allocator;
            if (id == H2_PAL_HTTP_E2E_STREAM_CHUNKED) {
                path = "/chunked"; callbacks.unknown_length = 1;
            }
            if (id == H2_PAL_HTTP_E2E_STREAM_NO_SCRATCH) {
                request.chunk_buf = NULL; request.chunk_buf_cap = 0u;
                callbacks.chunk_buf = NULL;
            }
            if (id == H2_PAL_HTTP_E2E_STREAM_ABORT) {
                callbacks.read_abort = 1; expected_rc = H2_PAL_ERR_IO;
                request.retry_count = 2;
            }
            if (id == H2_PAL_HTTP_E2E_CANCEL_STREAM) {
                callbacks.cancel_after_read = 1; expected_rc = H2_PAL_ERR_CLOSED;
                request.retry_count = 2;
            }
            break;
        case H2_PAL_HTTP_E2E_HEADER_ABORT:
            callbacks.header_abort = 1; expected_rc = H2_PAL_ERR_IO;
            request.retry_count = 2;
            break;
        case H2_PAL_HTTP_E2E_CANCEL_BEFORE:
            callbacks.cancel = 1; expected_rc = H2_PAL_ERR_CLOSED; break;
        case H2_PAL_HTTP_E2E_CANCEL_WAIT:
            path = "/slow-headers"; expected_rc = H2_PAL_ERR_CLOSED;
            CHECK(h2_pal_time_get_monotonic_ms(runtime->time, &callbacks.cancel_at_ms) == H2_PAL_OK);
            callbacks.cancel_at_ms += 2000u;
            break;
        case H2_PAL_HTTP_E2E_TIMEOUT_HEADERS:
        case H2_PAL_HTTP_E2E_TIMEOUT_BODY:
            path = id == H2_PAL_HTTP_E2E_TIMEOUT_HEADERS ? "/slow-headers" : "/slow-body";
            request.timeout_ms = 2000; expected_rc = H2_PAL_ERR_TIMEOUT; break;
        case H2_PAL_HTTP_E2E_TRUNCATED_BODY:
            path = "/truncated";
            request.response_buf = NULL; request.response_buf_cap = 0u;
            request.response_allocator = &allocator;
            expected_rc = H2_PAL_ERR_IO;
            break;
        case H2_PAL_HTTP_E2E_RETRY_RECOVER:
            path = "/retry/recover"; request.retry_count = 1; break;
        case H2_PAL_HTTP_E2E_RETRY_EXHAUSTED:
            path = "/retry/exhausted"; request.retry_count = 2; expected_status = 503; break;
        case H2_PAL_HTTP_E2E_RETRY_DEADLINE:
            path = "/retry/deadline"; request.retry_count = 4;
            request.timeout_ms = 5000; expected_rc = H2_PAL_ERR_TIMEOUT; break;
        case H2_PAL_HTTP_E2E_REDIRECT_RELATIVE:
            path = "/redirect/relative"; break;
        case H2_PAL_HTTP_E2E_REDIRECT_POST_303:
        case H2_PAL_HTTP_E2E_REDIRECT_POST_307:
            request.method = H2_PAL_HTTP_POST;
            request.body = upload; request.body_len = sizeof(upload);
            path = id == H2_PAL_HTTP_E2E_REDIRECT_POST_303 ? "/redirect/303" : "/redirect/307";
            callbacks.method = NULL;
            break;
        case H2_PAL_HTTP_E2E_HTTPS_TRUSTED:
            base = config->https_base; break;
        case H2_PAL_HTTP_E2E_HTTPS_UNTRUSTED:
            base = config->untrusted_https_base; check_body = 0; break;
        case H2_PAL_HTTP_E2E_INTERFACE_REJECTED:
            request.interface_name = "h2-e2e-interface-does-not-exist";
            check_body = 0; break;
        case H2_PAL_HTTP_E2E_INVALID_METHOD:
            request.method = (h2_pal_http_method_t)999;
            expected_rc = H2_PAL_ERR_INVALID_ARG; break;
        case H2_PAL_HTTP_E2E_INVALID_URL_SPAN:
            expected_rc = H2_PAL_ERR_INVALID_ARG; break;
        case H2_PAL_HTTP_E2E_INVALID_HEADER:
            header.name = (h2_pal_http_str_t){invalid_name, sizeof(invalid_name)};
            header.value = (h2_pal_http_str_t){"v", 1u};
            request.headers = &header; request.header_count = 1u;
            expected_rc = H2_PAL_ERR_INVALID_ARG; break;
        case H2_PAL_HTTP_E2E_DISCARD_BODY:
            request.response_buf = NULL; request.response_buf_cap = 0u;
            check_body = 0; break;
        case H2_PAL_HTTP_E2E_RESPONSE_FREE:
        case H2_PAL_HTTP_E2E_REPEATED_CLEANUP:
            request.response_buf = NULL; request.response_buf_cap = 0u;
            request.response_allocator = &allocator; break;
        case H2_PAL_HTTP_E2E_CASE_COUNT: CHECK(0); break;
    }
    int url_len = snprintf(url, sizeof(url), "%s%s", base, path);
    CHECK(url_len > 0 && (size_t)url_len + 8u < sizeof(url));
    request.url = (h2_pal_http_str_t){url, (size_t)url_len};
    memcpy(url + url_len, "!poison", 8u);
    if (id == H2_PAL_HTTP_E2E_INVALID_URL_SPAN) url[5] = '\0';
    CHECK(h2_pal_time_get_monotonic_ms(runtime->time, &started) == H2_PAL_OK);
    rc = h2_pal_http_request(http, &request, &response);
    CHECK(h2_pal_time_get_monotonic_ms(runtime->time, &finished) == H2_PAL_OK);
    result->elapsed_ms = finished - started;
    CHECK(finished >= started && result->elapsed_ms < (uint64_t)request.timeout_ms + 500u);
    if (id == H2_PAL_HTTP_E2E_HTTPS_UNTRUSTED ||
        id == H2_PAL_HTTP_E2E_INTERFACE_REJECTED) {
        CHECK(rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT);
        CHECK(response.body == NULL && callbacks.headers == 0u);
    } else {
        if (id == H2_PAL_HTTP_E2E_TRUNCATED_BODY)
            CHECK(rc == H2_PAL_ERR_IO || rc == H2_PAL_ERR_FORMAT || rc == H2_PAL_ERR_CLOSED);
        else CHECK(rc == expected_rc);
        CHECK(!callbacks.invalid);
        if (rc == H2_PAL_OK) {
            CHECK(response.status_code == expected_status);
            CHECK(callbacks.method_headers != 0u);
            if (request.read_cb != NULL) {
                CHECK(callbacks.received == HTTP_BODY_BYTES && callbacks.reads > 0u);
                CHECK(response.body_len == HTTP_BODY_BYTES && response.body == NULL && response.allocator == NULL);
                CHECK(tracked.allocations == 0u);
                for (size_t index = 0u; index < sizeof(output); ++index) CHECK(output[index] == 0xa5);
            } else if (check_body) {
                CHECK(body_matches(&response));
            } else if (id == H2_PAL_HTTP_E2E_DISCARD_BODY) {
                CHECK(response.body == NULL && response.body_len == HTTP_BODY_BYTES);
            } else {
                CHECK(response.body_len == 0u && response.allocator == NULL);
            }
            if (request.response_buf != NULL && request.read_cb == NULL && check_body)
                CHECK(response.body == request.response_buf && response.allocator == NULL);
            if (h2_pal_http_request_uses_allocator(&request)) {
                CHECK(response.allocator == &allocator && tracked.live == 1u);
            }
        } else {
            CHECK(response_empty(&response));
        }
    }
    CHECK(output[0] == 0xa5 && output[sizeof(output) - 1u] == 0xa5);
    if (id == H2_PAL_HTTP_E2E_CALLER_TOO_SMALL) CHECK(output[HTTP_BODY_BYTES] == 0xa5);
    if (id == H2_PAL_HTTP_E2E_BYTE_SPANS) CHECK(callbacks.marker_headers == 1u);
    if (id == H2_PAL_HTTP_E2E_HEAD) CHECK(response.content_length == HTTP_BODY_BYTES);
    if (id == H2_PAL_HTTP_E2E_STREAM_CHUNKED) CHECK(response.content_length == -1);
    if (id == H2_PAL_HTTP_E2E_RETRY_RECOVER) CHECK(callbacks.attempt == 2u);
    if (id == H2_PAL_HTTP_E2E_RETRY_EXHAUSTED) CHECK(callbacks.attempt == 3u);
    if (id == H2_PAL_HTTP_E2E_HEADER_ABORT) CHECK(callbacks.headers == 1u && callbacks.reads == 0u);
    if (id == H2_PAL_HTTP_E2E_CANCEL_BEFORE) CHECK(callbacks.headers == 0u && callbacks.reads == 0u);
    if (id == H2_PAL_HTTP_E2E_CANCEL_STREAM || id == H2_PAL_HTTP_E2E_STREAM_ABORT) CHECK(callbacks.reads == 1u);
    if (id == H2_PAL_HTTP_E2E_ALLOCATOR_FAILURE) CHECK(tracked.allocations != 0u && tracked.live == 0u);
    if (id == H2_PAL_HTTP_E2E_TIMEOUT_HEADERS || id == H2_PAL_HTTP_E2E_TIMEOUT_BODY ||
        id == H2_PAL_HTTP_E2E_RETRY_DEADLINE || id == H2_PAL_HTTP_E2E_CANCEL_WAIT)
        CHECK(result->elapsed_ms >= (id == H2_PAL_HTTP_E2E_RETRY_DEADLINE ? 4500u : 1500u) &&
              result->elapsed_ms < (id == H2_PAL_HTTP_E2E_RETRY_DEADLINE ? 5500u : 2500u));
    if (id == H2_PAL_HTTP_E2E_CALLBACK_QUIESCENCE || expected_rc != H2_PAL_OK) {
        unsigned count = callbacks.reads + callbacks.headers;
        CHECK(h2_pal_time_sleep_ms(runtime->time, 80u) == H2_PAL_OK);
        CHECK(callbacks.reads + callbacks.headers == count);
    }
    if (id == H2_PAL_HTTP_E2E_REPEATED_CLEANUP) {
        for (unsigned iteration = 0u; iteration < 24u; ++iteration) {
            h2_pal_http_response_free(http, &response);
            CHECK(tracked.live == 0u && !tracked.invalid_free);
            rc = h2_pal_http_request(http, &request, &response);
            CHECK(rc == H2_PAL_OK && body_matches(&response));
        }
    }
    result->passed = 1;
done:
    h2_pal_http_response_free(http, &response);
    h2_pal_http_response_free(http, &response);
    h2_pal_http_response_free(http, NULL);
    if (!response_empty(&response) || tracked.live != 0u || alias.live != 0u ||
        tracked.invalid_free || alias.invalid_free) {
        result->passed = 0;
        if (result->line == 0u) result->line = __LINE__;
        result->detail = H2_PAL_ERR_IO;
    }
    *retained += tracked.live + alias.live;
    /* Retained addresses cannot escape stack-owned allocator contexts. */
    for (size_t index = 0u; index < HTTP_ALLOCATION_SLOTS; ++index) {
        h2_pal_mem_free(runtime->mem, tracked.slots[index]);
        h2_pal_mem_free(runtime->mem, alias.slots[index]);
    }
}

int h2_pal_http_e2e_run(const h2_pal_http_e2e_config_t *config,
                      h2_pal_http_e2e_result_t *out_result) {
    if (config == NULL || out_result == NULL || config->runtime == NULL ||
        config->http_base == NULL || config->https_base == NULL ||
        config->untrusted_https_base == NULL) return H2_PAL_ERR_INVALID_ARG;
    memset(out_result, 0, sizeof(*out_result));
    const h2_runtime_t *runtime = config->runtime;
    int complete = runtime->http != NULL && runtime->http->vtable != NULL &&
        runtime->http->vtable->request != NULL && runtime->http->vtable->response_free != NULL &&
        runtime->mem != NULL && runtime->mem->vtable != NULL &&
        runtime->mem->vtable->alloc != NULL && runtime->mem->vtable->realloc != NULL &&
        runtime->mem->vtable->free != NULL && runtime->time != NULL;
    for (unsigned index = 0u; index < H2_PAL_HTTP_E2E_CASE_COUNT; ++index) {
        h2_pal_http_e2e_case_result_t *result = &out_result->cases[index];
        result->id = case_ids[index];
        if (!complete) {
            result->blocked = 1;
            result->detail = H2_PAL_ERR_UNSUPPORTED;
        } else {
            execute_case(config, (h2_pal_http_e2e_case_t)index, result,
                         &out_result->retained_allocations);
        }
        if (result->passed) ++out_result->passed;
        else if (result->blocked) ++out_result->blocked;
        else ++out_result->failed;
        if (config->report != NULL) config->report(config->report_user, result);
    }
    return out_result->passed == H2_PAL_HTTP_E2E_CASE_COUNT &&
        out_result->retained_allocations == 0u ? H2_PAL_OK : H2_PAL_ERR_IO;
}
