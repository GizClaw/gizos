#include "h2_fixture_api.h"
#include "h2_status_api.h"
#include "h2_yyjson_json.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct allocation_header {
    size_t marker;
} allocation_header_t;

typedef struct test_state {
    size_t live_allocations;
    bool request_seen;
    bool return_error;
    bool malformed_response;
    bool partial_decode_error;
    bool invalid_response_header;
    bool fail_next_allocation;
    bool fail_duplicate_response_header;
    bool ambiguous_one_of;
    bool append_uninitialized_header;
    int forced_http_result;
    int forced_status;
} test_state_t;

static const uint8_t upload_bytes[] =
    "\x01\x00\xff\r\n--h2-openapi-boundary\r\n"
    "--h2-openapi-boundary-0000000000000000\r\n";

static void *test_alloc(void *user, size_t length) {
    test_state_t *state = user;
    if (state->fail_next_allocation) {
        state->fail_next_allocation = false;
        return NULL;
    }
    allocation_header_t *header = malloc(sizeof(*header) + length);
    if (header == NULL) {
        return NULL;
    }
    header->marker = 0x743u;
    ++state->live_allocations;
    return header + 1;
}

static void *test_realloc(void *user, void *pointer, size_t length) {
    if (pointer == NULL) {
        return test_alloc(user, length);
    }
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    assert(header->marker == 0x743u);
    header = realloc(header, sizeof(*header) + length);
    if (header == NULL) {
        return NULL;
    }
    header->marker = 0x743u;
    return header + 1;
}

static void test_free(void *user, void *pointer) {
    if (pointer == NULL) {
        return;
    }
    test_state_t *state = user;
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    assert(header->marker == 0x743u);
    assert(state->live_allocations > 0u);
    --state->live_allocations;
    free(header);
}

static const h2_pal_mem_vtable_t test_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};

static bool bytes_equal(const void *left, size_t left_len, const char *right) {
    const size_t right_len = strlen(right);
    return left_len == right_len && memcmp(left, right, right_len) == 0;
}

static bool bytes_contain(
    const uint8_t *data,
    size_t data_len,
    const void *needle,
    size_t needle_len) {
    if (needle_len == 0u) {
        return true;
    }
    if (data == NULL || needle == NULL || needle_len > data_len) {
        return false;
    }
    for (size_t offset = 0u; offset <= data_len - needle_len; ++offset) {
        if (memcmp(data + offset, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static const h2_pal_http_header_t *find_header(
    const h2_pal_http_request_t *request,
    const char *name) {
    for (size_t index = 0u; index < request->header_count; ++index) {
        if (bytes_equal(
                request->headers[index].name.data,
                request->headers[index].name.len,
                name)) {
            return &request->headers[index];
        }
    }
    return NULL;
}

static int fake_http_request(
    void *user,
    const h2_pal_http_request_t *request,
    h2_pal_http_response_t *response) {
    test_state_t *state = user;
    if (state->forced_http_result != H2_PAL_OK) {
        return state->forced_http_result;
    }
    static const char items_url[] =
        "https://example.test/items?tag=alpha&tag=beta";
    if (request->method == H2_PAL_HTTP_GET &&
        request->url.len == sizeof(items_url) - 1u &&
        memcmp(request->url.data, items_url, sizeof(items_url) - 1u) == 0) {
        const h2_pal_http_header_t *authorization =
            find_header(request, "Authorization");
        assert(authorization != NULL);
        assert(bytes_equal(
            authorization->value.data,
            authorization->value.len,
            "Bearer token"));
        assert(request->body == NULL && request->body_len == 0u);
        static const char success_payload[] =
            "[{\"choice\":{\"a\":\"x\"},\"metadata\":{\"k\":1},"
            "\"name\":null}]";
        static const char ambiguous_payload[] =
            "[{\"choice\":{\"a\":\"x\",\"b\":true}}]";
        const char *payload = state->ambiguous_one_of
            ? ambiguous_payload
            : success_payload;
        const size_t payload_len = state->ambiguous_one_of
            ? sizeof(ambiguous_payload) - 1u
            : sizeof(success_payload) - 1u;
        assert(payload_len <= request->response_buf_cap);
        memcpy(request->response_buf, payload, payload_len);
        response->status_code = 200;
        response->content_length = (int64_t)payload_len;
        response->body = request->response_buf;
        response->body_len = payload_len;
        state->request_seen = true;
        return H2_PAL_OK;
    }

    static const char upload_url[] = "https://example.test/upload";
    if (request->method == H2_PAL_HTTP_POST &&
        request->url.len == sizeof(upload_url) - 1u &&
        memcmp(request->url.data, upload_url, sizeof(upload_url) - 1u) == 0) {
        const h2_pal_http_header_t *content_type =
            find_header(request, "Content-Type");
        assert(content_type != NULL);
        assert(bytes_equal(
            content_type->value.data,
            content_type->value.len,
            "multipart/form-data; boundary=h2-openapi-boundary-0000000000000001"));
        static const char dir_part[] =
            "Content-Disposition: form-data; name=\"dir\"\r\n\r\nassets\r\n";
        assert(bytes_contain(
            request->body,
            request->body_len,
            dir_part,
            sizeof(dir_part) - 1u));
        assert(bytes_contain(
            request->body,
            request->body_len,
            upload_bytes,
            sizeof(upload_bytes)));
        static const char closing[] =
            "--h2-openapi-boundary-0000000000000001--\r\n";
        assert(bytes_contain(
            request->body,
            request->body_len,
            closing,
            sizeof(closing) - 1u));
        response->status_code = 204;
        response->content_length = 0;
        state->request_seen = true;
        return H2_PAL_OK;
    }

    const char expected_url[] = "https://example.test/pets/a%20b?verbose=true";
    assert(request->url.len == sizeof(expected_url) - 1u);
    assert(memcmp(request->url.data, expected_url, sizeof(expected_url) - 1u) == 0);
    assert(request->header_count == 3u);
    assert(request->body != NULL);
    assert(request->body_len != 0u);
    assert(strstr((const char *)request->body, "\"name\":\"Milo\"") != NULL);
    assert(strstr((const char *)request->body, "\"kind\":\"cat\"") != NULL);

    if (request->response_header_cb != NULL) {
        const char *trace = state->invalid_response_header ? "invalid" : "trace-1";
        const int header_result = request->response_header_cb(
            request->response_header_user, request,
            (h2_pal_http_str_t){"X-Trace", 7u},
            (h2_pal_http_str_t){trace, strlen(trace)});
        if (header_result != H2_PAL_OK) {
            return header_result;
        }
        if (state->fail_duplicate_response_header) {
            state->fail_next_allocation = true;
            return request->response_header_cb(
                request->response_header_user, request,
                (h2_pal_http_str_t){"X-Trace", 7u},
                (h2_pal_http_str_t){"trace-1", 7u});
        }
    }

    static const char success_json[] =
        "{\"active\":true,\"age\":4,\"kind\":\"cat\","
        "\"name\":\"Milo\",\"tags\":[\"indoor\"]}";
    static const char error_json[] = "{\"message\":\"invalid pet\"}";
    static const char malformed_json[] = "{";
    static const char partial_decode_json[] =
        "{\"active\":true,\"age\":4,\"kind\":\"cat\","
        "\"name\":\"Milo\",\"tags\":[123]}";
    const char *payload = state->malformed_response ? malformed_json :
        (state->partial_decode_error ? partial_decode_json :
        (state->return_error ? error_json : success_json));
    const size_t payload_len = state->malformed_response ? sizeof(malformed_json) - 1u :
        (state->partial_decode_error ? sizeof(partial_decode_json) - 1u :
        (state->return_error ? sizeof(error_json) - 1u : sizeof(success_json) - 1u));
    assert(payload_len <= request->response_buf_cap);
    memcpy(request->response_buf, payload, payload_len);
    response->status_code = state->forced_status != 0 ? state->forced_status : (state->return_error ? 400 : 200);
    response->content_length = (int64_t)payload_len;
    response->body = request->response_buf;
    response->body_len = payload_len;
    state->request_seen = true;
    return H2_PAL_OK;
}

static void fake_http_response_free(void *user, h2_pal_http_response_t *response) {
    (void)user;
    h2_pal_http_response_reset(response);
}

static const h2_pal_http_vtable_t test_http_vtable = {
    .request = fake_http_request,
    .response_free = fake_http_response_free,
};

static h2_fixture_result_t test_header_callback(
    void *user,
    h2_pal_http_header_t *headers,
    size_t capacity,
    size_t *in_out_count) {
    test_state_t *state = user;
    assert(headers != NULL);
    assert(in_out_count != NULL);
    assert(*in_out_count <= capacity);
    if (state->append_uninitialized_header) {
        assert(*in_out_count < capacity);
        assert(headers[*in_out_count].name.data == NULL);
        assert(headers[*in_out_count].name.len == 0u);
        assert(headers[*in_out_count].value.data == NULL);
        assert(headers[*in_out_count].value.len == 0u);
        ++*in_out_count;
    }
    return H2_FIXTURE_OK;
}

int main(void) {
    assert(h2_status_client_init(NULL, NULL) == H2_STATUS_ERR_INVALID_ARG);
    test_state_t state = {0};
    const h2_pal_mem_api_t mem = {.user = &state, .vtable = &test_mem_vtable};
    h2_yyjson_json_t *json_provider = NULL;
    assert(h2_yyjson_json_create(&mem, &json_provider) == H2_PAL_OK);
    const h2_pal_http_api_t http = {.user = &state, .vtable = &test_http_vtable};
    const h2_fixture_client_config_t config = {
        .http = &http,
        .json = h2_yyjson_json_api(json_provider),
        .mem = &mem,
        .base_url = "https://example.test",
        .max_url_bytes = 128u,
        .timeout_ms = 1000,
        .max_response_bytes = 256u,
        .max_request_bytes = 512u,
        .max_string_bytes = 128u,
        .max_array_items = 16u,
        .bearer_token = {"token", 5u},
        .header_callback = test_header_callback,
        .header_user = &state,
        .max_extra_headers = 1u,
    };
    h2_fixture_client_t client = {0};
    h2_fixture_client_config_t invalid_config = config;
    invalid_config.base_url = "";
    assert(h2_fixture_client_init(&client, &invalid_config) == H2_FIXTURE_ERR_INVALID_ARG);
    invalid_config = config;
    invalid_config.max_url_bytes = H2_FIXTURE_MAX_URL_BYTES + 1u;
    assert(h2_fixture_client_init(&client, &invalid_config) == H2_FIXTURE_ERR_INVALID_ARG);
    const h2_pal_http_vtable_t incomplete_http_vtable = {.request = fake_http_request};
    const h2_pal_http_api_t incomplete_http = {.user = &state, .vtable = &incomplete_http_vtable};
    invalid_config = config;
    invalid_config.http = &incomplete_http;
    assert(h2_fixture_client_init(&client, &invalid_config) == H2_FIXTURE_ERR_INVALID_ARG);
    assert(h2_fixture_client_init(&client, &config) == H2_FIXTURE_OK);

    h2_fixture_string_t query_tags[] = {
        {"alpha", 5u},
        {"beta", 4u},
    };
    const h2_fixture_list_items_request_t list_request = {
        .has_tag = true,
        .tag = {.items = query_tags, .count = 2u},
    };
    h2_fixture_list_items_response_t list_response = {0};
    assert(h2_fixture_list_items(
               &client, &list_request, &list_response) == H2_FIXTURE_OK);
    assert(list_response.status_code == 200 && list_response.has_body);
    assert(list_response.body.status_200.value.count == 1u);
    h2_fixture_value_t *listed_value =
        list_response.body.status_200.value.items[0];
    assert(listed_value != NULL);
    assert(listed_value->has_name && listed_value->is_name_null);
    assert(listed_value->has_metadata);
    assert(bytes_contain(
        (const uint8_t *)listed_value->metadata.data,
        listed_value->metadata.len,
        "\"k\":1",
        5u));
    assert(listed_value->has_choice && listed_value->choice != NULL);
    assert(listed_value->choice->kind == H2_FIXTURE_VALUE_CHOICE_A);
    assert(listed_value->choice->value.a != NULL);
    assert(bytes_equal(
        listed_value->choice->value.a->a.data,
        listed_value->choice->value.a->a.len,
        "x"));
    h2_fixture_list_items_response_deinit(&list_response);
    state.ambiguous_one_of = true;
    assert(h2_fixture_list_items(
               &client, &list_request, &list_response) == H2_FIXTURE_ERR_JSON);
    assert(list_response.status_code == 200);
    h2_fixture_list_items_response_deinit(&list_response);
    state.ambiguous_one_of = false;

    const h2_fixture_upload_request_body_t upload_body = {
        .dir = {"assets", 6u},
        .file = {upload_bytes, sizeof(upload_bytes)},
    };
    const h2_fixture_upload_request_t upload_request = {.body = &upload_body};
    h2_fixture_upload_response_t upload_response = {0};
    assert(h2_fixture_upload(
               &client, &upload_request, &upload_response) == H2_FIXTURE_OK);
    assert(upload_response.status_code == 204);
    h2_fixture_upload_response_deinit(&upload_response);
    state.request_seen = false;

    h2_fixture_string_t tags[] = {{"indoor", 6u}};
    const h2_fixture_pet_t pet = {
        .active = true,
        .age = 4,
        .kind = {"cat", 3u},
        .name = {"Milo", 4u},
        .tags = {.items = tags, .count = 1u},
    };
    const h2_fixture_update_pet_request_t request = {
        .x_device_id = {"device-1", 8u},
        .pet_id = {"a b", 3u},
        .has_verbose = true,
        .verbose = true,
        .body = &pet,
    };
    h2_fixture_update_pet_response_t response = {0};

    h2_fixture_update_pet_request_t invalid_request = request;
    state.append_uninitialized_header = true;
    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_ERR_INVALID_ARG);
    assert(!state.request_seen);
    h2_fixture_update_pet_response_deinit(&response);
    state.append_uninitialized_header = false;

    invalid_request.x_device_id = (h2_fixture_string_t){"bad\r\nheader", 11u};
    assert(h2_fixture_update_pet(&client, &invalid_request, &response) == H2_FIXTURE_ERR_INVALID_ARG);
    assert(!state.request_seen);
    h2_fixture_update_pet_response_deinit(&response);
    h2_fixture_update_pet_response_deinit(&response);

    static const uint8_t invalid_utf8[] = {0xc0u, 0xafu};
    invalid_request = request;
    invalid_request.pet_id = (h2_fixture_string_t){
        (const char *)invalid_utf8,
        sizeof(invalid_utf8),
    };
    assert(h2_fixture_update_pet(&client, &invalid_request, &response) == H2_FIXTURE_ERR_INVALID_ARG);
    assert(!state.request_seen);
    h2_fixture_update_pet_response_deinit(&response);

    state.return_error = false;
    state.forced_status = 201;
    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_ERR_UNEXPECTED_STATUS);
    assert(response.status_code == 201);
    h2_fixture_update_pet_response_deinit(&response);

    state.forced_status = 0;
    state.malformed_response = true;
    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_ERR_JSON);
    assert(response.status_code == 200);
    h2_fixture_update_pet_response_deinit(&response);
    state.malformed_response = false;

    state.partial_decode_error = true;
    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_ERR_JSON);
    assert(response.status_code == 200);
    h2_fixture_update_pet_response_deinit(&response);
    state.partial_decode_error = false;

    state.forced_http_result = H2_PAL_ERR_NO_SPACE;
    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_ERR_NO_SPACE);
    h2_fixture_update_pet_response_deinit(&response);
    state.forced_http_result = H2_PAL_ERR_NO_MEMORY;
    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_ERR_NO_MEMORY);
    h2_fixture_update_pet_response_deinit(&response);
    state.forced_http_result = H2_PAL_ERR_UNSUPPORTED;
    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_ERR_UNSUPPORTED);
    h2_fixture_update_pet_response_deinit(&response);
    state.forced_http_result = H2_PAL_ERR_IO;
    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_ERR_HTTP);
    h2_fixture_update_pet_response_deinit(&response);
    state.forced_http_result = H2_PAL_OK;

    state.invalid_response_header = true;
    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_ERR_HTTP);
    assert(!response.has_x_trace);
    h2_fixture_update_pet_response_deinit(&response);
    state.invalid_response_header = false;

    state.fail_duplicate_response_header = true;
    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_ERR_NO_MEMORY);
    assert(!response.has_x_trace);
    h2_fixture_update_pet_response_deinit(&response);
    state.fail_duplicate_response_header = false;

    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_OK);
    assert(state.request_seen);
    assert(response.status_code == 200);
    assert(response.has_body);
    assert(response.body.status_200.name.len == 4u);
    assert(memcmp(response.body.status_200.name.data, "Milo", 4u) == 0);
    assert(response.body.status_200.tags.count == 1u);
    assert(response.has_x_trace);
    assert(response.x_trace.len == 7u);
    assert(memcmp(response.x_trace.data, "trace-1", 7u) == 0);

    state.return_error = true;
    /* Reusing a live response releases the previous success body first. */
    assert(h2_fixture_update_pet(&client, &request, &response) == H2_FIXTURE_ERR_HTTP_STATUS);
    assert(response.status_code == 400);
    assert(response.has_body);
    assert(response.body.status_400.message.len == 11u);
    assert(memcmp(response.body.status_400.message.data, "invalid pet", 11u) == 0);
    h2_fixture_update_pet_response_deinit(&response);

    assert(h2_yyjson_json_destroy(&json_provider) == H2_PAL_OK);
    assert(state.live_allocations == 0u);
    return 0;
}
