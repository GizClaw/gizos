#include "h2_pal_json_conformance.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct allocation_header {
    size_t size;
} allocation_header_t;

typedef struct allocator_fixture {
    size_t calls;
    size_t fail_at;
    size_t live_allocations;
    size_t live_bytes;
    size_t peak_live_bytes;
} allocator_fixture_t;

typedef struct provider_fixture {
    allocator_fixture_t allocator;
    h2_pal_mem_api_t mem;
    void *provider;
    const h2_pal_json_api_t *api;
} provider_fixture_t;

static bool should_fail(allocator_fixture_t *fixture) {
    const size_t call = fixture->calls++;
    return call == fixture->fail_at;
}

static void *fixture_alloc(void *user, size_t size) {
    allocator_fixture_t *fixture = user;
    if (should_fail(fixture) || size > SIZE_MAX - sizeof(allocation_header_t)) {
        return NULL;
    }
    allocation_header_t *header = malloc(sizeof(*header) + size);
    if (header == NULL) return NULL;
    header->size = size;
    ++fixture->live_allocations;
    fixture->live_bytes += size;
    if (fixture->live_bytes > fixture->peak_live_bytes) {
        fixture->peak_live_bytes = fixture->live_bytes;
    }
    return header + 1;
}

static void fixture_free(void *user, void *pointer) {
    allocator_fixture_t *fixture = user;
    if (pointer == NULL) return;
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    assert(fixture->live_allocations > 0u);
    assert(fixture->live_bytes >= header->size);
    --fixture->live_allocations;
    fixture->live_bytes -= header->size;
    free(header);
}

static void *fixture_realloc(
    void *user,
    void *pointer,
    size_t size) {
    allocator_fixture_t *fixture = user;
    if (pointer == NULL) return fixture_alloc(user, size);
    if (size == 0u) {
        fixture_free(user, pointer);
        return NULL;
    }
    if (should_fail(fixture) || size > SIZE_MAX - sizeof(allocation_header_t)) {
        return NULL;
    }
    allocation_header_t *old_header = (allocation_header_t *)pointer - 1;
    const size_t old_size = old_header->size;
    allocation_header_t *new_header = realloc(
        old_header, sizeof(*new_header) + size);
    if (new_header == NULL) return NULL;
    new_header->size = size;
    fixture->live_bytes -= old_size;
    fixture->live_bytes += size;
    if (fixture->live_bytes > fixture->peak_live_bytes) {
        fixture->peak_live_bytes = fixture->live_bytes;
    }
    return new_header + 1;
}

static const h2_pal_mem_vtable_t fixture_mem_vtable = {
    .alloc = fixture_alloc,
    .realloc = fixture_realloc,
    .free = fixture_free,
};

static void init_provider_fixture(provider_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->allocator.fail_at = SIZE_MAX;
    fixture->mem.user = &fixture->allocator;
    fixture->mem.vtable = &fixture_mem_vtable;
}

static void create_provider(
    const h2_pal_json_test_provider_t *factory,
    provider_fixture_t *fixture) {
    init_provider_fixture(fixture);
    assert(factory->create(
               &fixture->mem, &fixture->provider, &fixture->api) == H2_PAL_OK);
    assert(fixture->provider != NULL);
    assert(fixture->api != NULL);
}

static void destroy_provider(
    const h2_pal_json_test_provider_t *factory,
    provider_fixture_t *fixture) {
    assert(factory->destroy(&fixture->provider) == H2_PAL_OK);
    assert(fixture->provider == NULL);
    assert(fixture->allocator.live_allocations == 0u);
    assert(fixture->allocator.live_bytes == 0u);
}

static void test_parse_and_query(
    const h2_pal_json_test_provider_t *factory,
    size_t *out_peak) {
    provider_fixture_t fixture;
    create_provider(factory, &fixture);
    static const char input[] =
        "{\"name\":\"\\u96ea\\n\",\"raw\":\"雪\","
        "\"pair\":\"\\ud83d\\ude00\",\"integer\":7,"
        "\"items\":[null,true,3.5,{\"\":\"ok\"}]}";
    h2_pal_json_document_t *document = NULL;
    assert(h2_pal_json_document_parse(
               fixture.api, (const uint8_t *)input, strlen(input), NULL,
               &document) == H2_PAL_OK);
    h2_pal_json_value_t *root = NULL;
    assert(h2_pal_json_document_root(fixture.api, document, &root) == H2_PAL_OK);
    h2_pal_json_type_t type = H2_PAL_JSON_TYPE_INVALID;
    assert(h2_pal_json_value_type(fixture.api, root, &type) == H2_PAL_OK);
    assert(type == H2_PAL_JSON_TYPE_OBJECT);

    size_t object_size = 0u;
    assert(h2_pal_json_object_size(
               fixture.api, root, &object_size) == H2_PAL_OK);
    assert(object_size == 5u);
    bool saw_items = false;
    for (size_t index = 0u; index < object_size; ++index) {
        h2_pal_json_string_view_t key = {0};
        h2_pal_json_value_t *entry = NULL;
        assert(h2_pal_json_object_entry(
                   fixture.api, root, index, &key, &entry) == H2_PAL_OK);
        assert(key.data != NULL && entry != NULL);
        if (key.len == 5u && memcmp(key.data, "items", 5u) == 0) {
            saw_items = true;
            assert(h2_pal_json_value_type(
                       fixture.api, entry, &type) == H2_PAL_OK);
            assert(type == H2_PAL_JSON_TYPE_ARRAY);
        }
    }
    assert(saw_items);
    h2_pal_json_string_view_t missing_key = {
        .data = (const char *)(uintptr_t)1u,
        .len = 1u,
    };
    h2_pal_json_value_t *missing_entry =
        (h2_pal_json_value_t *)(uintptr_t)1u;
    assert(h2_pal_json_object_entry(
               fixture.api, root, object_size, &missing_key,
               &missing_entry) == H2_PAL_ERR_NOT_FOUND);
    assert(missing_key.data == NULL && missing_key.len == 0u);
    assert(missing_entry == NULL);

    h2_pal_json_value_t *name = NULL;
    assert(h2_pal_json_object_get(
               fixture.api, root, "name", 4u, &name) == H2_PAL_OK);
    h2_pal_json_string_view_t string = {0};
    assert(h2_pal_json_value_get_string(
               fixture.api, name, &string) == H2_PAL_OK);
    assert(string.len == strlen("雪\n"));
    assert(memcmp(string.data, "雪\n", string.len) == 0);

    h2_pal_json_value_t *raw = NULL;
    assert(h2_pal_json_object_get(
               fixture.api, root, "raw", 3u, &raw) == H2_PAL_OK);
    assert(h2_pal_json_value_get_string(
               fixture.api, raw, &string) == H2_PAL_OK);
    assert(string.len == strlen("雪"));
    assert(memcmp(string.data, "雪", string.len) == 0);
    h2_pal_json_value_t *pair = NULL;
    assert(h2_pal_json_object_get(
               fixture.api, root, "pair", 4u, &pair) == H2_PAL_OK);
    assert(h2_pal_json_value_get_string(
               fixture.api, pair, &string) == H2_PAL_OK);
    assert(string.len == 4u);

    h2_pal_json_value_t *items = NULL;
    assert(h2_pal_json_object_get(
               fixture.api, root, "items", 5u, &items) == H2_PAL_OK);
    size_t size = 0u;
    assert(h2_pal_json_array_size(fixture.api, items, &size) == H2_PAL_OK);
    assert(size == 4u);
    assert(h2_pal_json_object_size(
               fixture.api, items, &object_size) == H2_PAL_ERR_INVALID_STATE);
    assert(object_size == 0u);
    h2_pal_json_value_t *number = NULL;
    assert(h2_pal_json_array_get(
               fixture.api, items, 2u, &number) == H2_PAL_OK);
    double number_value = 0.0;
    assert(h2_pal_json_value_get_number(
               fixture.api, number, &number_value) == H2_PAL_OK);
    assert(number_value == 3.5);
    h2_pal_json_value_t *boolean = NULL;
    assert(h2_pal_json_array_get(
               fixture.api, items, 1u, &boolean) == H2_PAL_OK);
    bool boolean_value = false;
    assert(h2_pal_json_value_get_boolean(
               fixture.api, boolean, &boolean_value) == H2_PAL_OK);
    assert(boolean_value);
    assert(h2_pal_json_value_type(fixture.api, boolean, &type) == H2_PAL_OK);
    assert(type == H2_PAL_JSON_TYPE_BOOLEAN);
    h2_pal_json_value_t *null_value = NULL;
    assert(h2_pal_json_array_get(
               fixture.api, items, 0u, &null_value) == H2_PAL_OK);
    assert(h2_pal_json_value_type(
               fixture.api, null_value, &type) == H2_PAL_OK);
    assert(type == H2_PAL_JSON_TYPE_NULL);
    h2_pal_json_value_t *integer = NULL;
    assert(h2_pal_json_object_get(
               fixture.api, root, "integer", 7u, &integer) == H2_PAL_OK);
    assert(h2_pal_json_value_get_number(
               fixture.api, integer, &number_value) == H2_PAL_OK);
    assert(number_value == 7.0);
    assert(h2_pal_json_array_get(
               fixture.api, items, 99u, &number) == H2_PAL_ERR_NOT_FOUND);
    assert(number == NULL);
    assert(h2_pal_json_object_get(
               fixture.api, root, "missing", 7u, &number) ==
           H2_PAL_ERR_NOT_FOUND);
    assert(number == NULL);
    assert(h2_pal_json_value_get_boolean(
               fixture.api, name, &(bool){false}) == H2_PAL_ERR_INVALID_STATE);

    h2_pal_json_buffer_t buffer = {0};
    assert(h2_pal_json_document_serialize(
               fixture.api, document, &buffer) == H2_PAL_OK);
    assert(buffer.data != NULL && buffer.len > 0u && buffer.data[buffer.len] == 0u);
    h2_pal_json_document_t *roundtrip = NULL;
    assert(h2_pal_json_document_parse(
               fixture.api, buffer.data, buffer.len, NULL,
               &roundtrip) == H2_PAL_OK);
    assert(h2_pal_json_document_destroy(
               fixture.api, &roundtrip) == H2_PAL_OK);
    assert(h2_pal_json_buffer_release(fixture.api, &buffer) == H2_PAL_OK);
    assert(buffer.data == NULL && buffer.len == 0u);
    assert(h2_pal_json_document_destroy(
               fixture.api, &document) == H2_PAL_OK);
    *out_peak = fixture.allocator.peak_live_bytes;
    destroy_provider(factory, &fixture);
}

static void expect_parse_result(
    const h2_pal_json_api_t *api,
    const uint8_t *data,
    size_t len,
    const h2_pal_json_limits_t *limits,
    h2_pal_result_t expected) {
    h2_pal_json_document_t *document = (h2_pal_json_document_t *)(uintptr_t)1u;
    assert(h2_pal_json_document_parse(
               api, data, len, limits, &document) == expected);
    assert(document == NULL);
}

static void test_strict_input_and_limits(
    const h2_pal_json_test_provider_t *factory) {
    provider_fixture_t fixture;
    create_provider(factory, &fixture);
    static const char *invalid[] = {
        "", "{", "[1,]", "{\"a\":1,\"a\":2}", "true false",
        "\xef\xbb\xbf{}", "\"\\u0000\"", "\"\\ud800\"", "1e999",
        "\"\\udc00\"", "\"\\ud800x\"", "\"\\q\"", "/*x*/{}",
    };
    for (size_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        expect_parse_result(
            fixture.api, (const uint8_t *)invalid[index], strlen(invalid[index]),
            NULL, H2_PAL_ERR_FORMAT);
    }
    const uint8_t invalid_utf8[] = {'"', 0xc0u, 0x80u, '"'};
    expect_parse_result(
        fixture.api, invalid_utf8, sizeof(invalid_utf8), NULL,
        H2_PAL_ERR_FORMAT);

    h2_pal_json_limits_t limits = {
        .max_document_bytes = H2_PAL_JSON_DEFAULT_MAX_DOCUMENT_BYTES,
        .max_depth = 2u,
        .max_values = H2_PAL_JSON_DEFAULT_MAX_VALUES,
    };
    expect_parse_result(
        fixture.api, (const uint8_t *)"[[[]]]", 6u, &limits,
        H2_PAL_ERR_NO_SPACE);
    limits.max_depth = H2_PAL_JSON_DEFAULT_MAX_DEPTH;
    limits.max_values = 2u;
    expect_parse_result(
        fixture.api, (const uint8_t *)"[1,2]", 5u, &limits,
        H2_PAL_ERR_NO_SPACE);
    limits.max_values = H2_PAL_JSON_DEFAULT_MAX_VALUES;
    limits.max_document_bytes = 2u;
    expect_parse_result(
        fixture.api, (const uint8_t *)"\"x\"", 3u, &limits,
        H2_PAL_ERR_NO_SPACE);
    limits.max_document_bytes = 0u;
    expect_parse_result(
        fixture.api, (const uint8_t *)"null", 4u, &limits,
        H2_PAL_ERR_INVALID_ARG);
    limits = (h2_pal_json_limits_t){
        .max_document_bytes = H2_PAL_JSON_DEFAULT_MAX_DOCUMENT_BYTES + 1u,
        .max_depth = H2_PAL_JSON_DEFAULT_MAX_DEPTH,
        .max_values = H2_PAL_JSON_DEFAULT_MAX_VALUES,
    };
    expect_parse_result(
        fixture.api, (const uint8_t *)"null", 4u, &limits,
        H2_PAL_ERR_INVALID_ARG);
    limits.max_document_bytes = H2_PAL_JSON_DEFAULT_MAX_DOCUMENT_BYTES;
    limits.max_depth = H2_PAL_JSON_DEFAULT_MAX_DEPTH + 1u;
    expect_parse_result(
        fixture.api, (const uint8_t *)"null", 4u, &limits,
        H2_PAL_ERR_INVALID_ARG);
    limits.max_depth = H2_PAL_JSON_DEFAULT_MAX_DEPTH;
    limits.max_values = H2_PAL_JSON_DEFAULT_MAX_VALUES + 1u;
    expect_parse_result(
        fixture.api, (const uint8_t *)"null", 4u, &limits,
        H2_PAL_ERR_INVALID_ARG);
    destroy_provider(factory, &fixture);
}

static void test_mutation_and_ownership(
    const h2_pal_json_test_provider_t *factory) {
    provider_fixture_t fixture;
    create_provider(factory, &fixture);
    h2_pal_json_document_t *document = NULL;
    assert(h2_pal_json_document_create(
               fixture.api, NULL, &document) == H2_PAL_OK);
    h2_pal_json_value_t *root = NULL;
    h2_pal_json_value_t *array = NULL;
    h2_pal_json_value_t *old_name = NULL;
    h2_pal_json_value_t *new_name = NULL;
    assert(h2_pal_json_value_create_object(
               fixture.api, document, &root) == H2_PAL_OK);
    assert(h2_pal_json_document_set_root(
               fixture.api, document, root) == H2_PAL_OK);
    assert(h2_pal_json_value_create_array(
               fixture.api, document, &array) == H2_PAL_OK);
    assert(h2_pal_json_object_set(
               fixture.api, root, "items", 5u, array) == H2_PAL_OK);

    h2_pal_json_value_t *boolean = NULL;
    h2_pal_json_value_t *number = NULL;
    h2_pal_json_value_t *null_value = NULL;
    assert(h2_pal_json_value_create_boolean(
               fixture.api, document, true, &boolean) == H2_PAL_OK);
    assert(h2_pal_json_value_create_number(
               fixture.api, document, 42.25, &number) == H2_PAL_OK);
    assert(h2_pal_json_value_create_null(
               fixture.api, document, &null_value) == H2_PAL_OK);
    h2_pal_json_type_t type = H2_PAL_JSON_TYPE_INVALID;
    assert(h2_pal_json_value_type(
               fixture.api, null_value, &type) == H2_PAL_OK);
    assert(type == H2_PAL_JSON_TYPE_NULL);
    assert(h2_pal_json_array_append(
               fixture.api, array, boolean) == H2_PAL_OK);
    assert(h2_pal_json_array_append(
               fixture.api, array, number) == H2_PAL_OK);
    assert(h2_pal_json_array_append(
               fixture.api, array, null_value) == H2_PAL_OK);
    assert(h2_pal_json_array_append(
               fixture.api, array, boolean) == H2_PAL_ERR_INVALID_STATE);

    assert(h2_pal_json_value_create_string(
               fixture.api, document, "old", 3u, &old_name) == H2_PAL_OK);
    assert(h2_pal_json_object_set(
               fixture.api, root, "name", 4u, old_name) == H2_PAL_OK);
    assert(h2_pal_json_value_create_string(
               fixture.api, document, "new", 3u, &new_name) == H2_PAL_OK);
    assert(h2_pal_json_object_set(
               fixture.api, root, "name", 4u, new_name) == H2_PAL_OK);
    h2_pal_json_string_view_t view = {0};
    assert(h2_pal_json_value_get_string(
               fixture.api, old_name, &view) == H2_PAL_ERR_INVALID_STATE);
    assert(view.data == NULL && view.len == 0u);

    h2_pal_json_value_t *empty = NULL;
    assert(h2_pal_json_value_create_string(
               fixture.api, document, NULL, 0u, &empty) == H2_PAL_OK);
    assert(h2_pal_json_object_set(
               fixture.api, root, NULL, 0u, empty) == H2_PAL_OK);
    assert(h2_pal_json_object_get(
               fixture.api, root, NULL, 0u, &empty) == H2_PAL_OK);

    h2_pal_json_value_t *cycle_object = NULL;
    h2_pal_json_value_t *cycle_array = NULL;
    assert(h2_pal_json_value_create_object(
               fixture.api, document, &cycle_object) == H2_PAL_OK);
    assert(h2_pal_json_value_create_array(
               fixture.api, document, &cycle_array) == H2_PAL_OK);
    assert(h2_pal_json_object_set(
               fixture.api, cycle_object, "a", 1u,
               cycle_array) == H2_PAL_OK);
    assert(h2_pal_json_array_append(
               fixture.api, cycle_array,
               cycle_object) == H2_PAL_ERR_INVALID_ARG);

    h2_pal_json_document_t *other_document = NULL;
    h2_pal_json_value_t *other_value = NULL;
    assert(h2_pal_json_document_create(
               fixture.api, NULL, &other_document) == H2_PAL_OK);
    assert(h2_pal_json_value_create_null(
               fixture.api, other_document, &other_value) == H2_PAL_OK);
    assert(h2_pal_json_array_append(
               fixture.api, array, other_value) == H2_PAL_ERR_INVALID_ARG);

    h2_pal_json_buffer_t buffer = {0};
    assert(h2_pal_json_document_serialize(
               fixture.api, document, &buffer) == H2_PAL_OK);
    assert(strstr((const char *)buffer.data, "\"name\":\"new\"") != NULL);
    assert(h2_pal_json_buffer_release(fixture.api, &buffer) == H2_PAL_OK);
    assert(h2_pal_json_document_destroy(
               fixture.api, &other_document) == H2_PAL_OK);
    assert(h2_pal_json_document_destroy(
               fixture.api, &document) == H2_PAL_OK);
    destroy_provider(factory, &fixture);
}

static void test_immutable_and_construction_validation(
    const h2_pal_json_test_provider_t *factory) {
    provider_fixture_t fixture;
    create_provider(factory, &fixture);
    static const char input[] = "[1]";
    h2_pal_json_document_t *parsed = NULL;
    h2_pal_json_value_t *parsed_root = NULL;
    assert(h2_pal_json_document_parse(
               fixture.api, (const uint8_t *)input, strlen(input), NULL,
               &parsed) == H2_PAL_OK);
    assert(h2_pal_json_document_root(
               fixture.api, parsed, &parsed_root) == H2_PAL_OK);
    h2_pal_json_value_t *parsed_item = NULL;
    assert(h2_pal_json_array_get(
               fixture.api, parsed_root, 0u, &parsed_item) == H2_PAL_OK);
    assert(h2_pal_json_array_append(
               fixture.api, parsed_root, parsed_item) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_json_document_destroy(
               fixture.api, &parsed) == H2_PAL_OK);

    h2_pal_json_document_t *document = NULL;
    assert(h2_pal_json_document_create(
               fixture.api, NULL, &document) == H2_PAL_OK);
    h2_pal_json_value_t *value = (h2_pal_json_value_t *)(uintptr_t)1u;
    const uint8_t invalid_utf8[] = {0xc0u, 0x80u};
    assert(h2_pal_json_value_create_string(
               fixture.api, document, (const char *)invalid_utf8,
               sizeof(invalid_utf8),
               &value) == H2_PAL_ERR_INVALID_ARG);
    assert(value == NULL);
    const char embedded_nul[] = {'a', '\0', 'b'};
    assert(h2_pal_json_value_create_string(
               fixture.api, document, embedded_nul, sizeof(embedded_nul),
               &value) == H2_PAL_ERR_INVALID_ARG);
    assert(value == NULL);
    assert(h2_pal_json_value_create_number(
               fixture.api, document, INFINITY,
               &value) == H2_PAL_ERR_INVALID_ARG);
    assert(value == NULL);
    const char one_byte = 'x';
    assert(h2_pal_json_value_create_string(
               fixture.api, document, &one_byte, SIZE_MAX,
               &value) == H2_PAL_ERR_INVALID_ARG);
    assert(value == NULL);

    h2_pal_json_value_t *object = NULL;
    h2_pal_json_value_t *detached = NULL;
    assert(h2_pal_json_value_create_object(
               fixture.api, document, &object) == H2_PAL_OK);
    assert(h2_pal_json_value_create_null(
               fixture.api, document, &detached) == H2_PAL_OK);
    assert(h2_pal_json_object_get(
               fixture.api, object, &one_byte, SIZE_MAX,
               &value) == H2_PAL_ERR_INVALID_ARG);
    assert(value == NULL);
    assert(h2_pal_json_object_set(
               fixture.api, object, &one_byte, SIZE_MAX,
               detached) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_json_object_set(
               fixture.api, object, &one_byte, 1u,
               detached) == H2_PAL_OK);
    assert(h2_pal_json_document_destroy(
               fixture.api, &document) == H2_PAL_OK);
    destroy_provider(factory, &fixture);
}

static void test_mutation_limits(
    const h2_pal_json_test_provider_t *factory) {
    provider_fixture_t fixture;
    create_provider(factory, &fixture);
    const h2_pal_json_limits_t limits = {
        .max_document_bytes = H2_PAL_JSON_DEFAULT_MAX_DOCUMENT_BYTES,
        .max_depth = 2u,
        .max_values = 3u,
    };
    h2_pal_json_document_t *document = NULL;
    h2_pal_json_value_t *root = NULL;
    h2_pal_json_value_t *child = NULL;
    h2_pal_json_value_t *grandchild = NULL;
    assert(h2_pal_json_document_create(
               fixture.api, &limits, &document) == H2_PAL_OK);
    assert(h2_pal_json_value_create_array(
               fixture.api, document, &root) == H2_PAL_OK);
    assert(h2_pal_json_document_set_root(
               fixture.api, document, root) == H2_PAL_OK);
    assert(h2_pal_json_value_create_array(
               fixture.api, document, &child) == H2_PAL_OK);
    assert(h2_pal_json_array_append(
               fixture.api, root, child) == H2_PAL_OK);
    assert(h2_pal_json_value_create_null(
               fixture.api, document, &grandchild) == H2_PAL_OK);
    assert(h2_pal_json_array_append(
               fixture.api, child, grandchild) == H2_PAL_ERR_NO_SPACE);
    h2_pal_json_value_t *excess = (h2_pal_json_value_t *)(uintptr_t)1u;
    assert(h2_pal_json_value_create_null(
               fixture.api, document, &excess) == H2_PAL_ERR_NO_SPACE);
    assert(excess == NULL);
    assert(h2_pal_json_document_destroy(
               fixture.api, &document) == H2_PAL_OK);
    destroy_provider(factory, &fixture);
}

static void test_output_limit(const h2_pal_json_test_provider_t *factory) {
    provider_fixture_t fixture;
    create_provider(factory, &fixture);
    const h2_pal_json_limits_t limits = {
        .max_document_bytes = 4u,
        .max_depth = H2_PAL_JSON_DEFAULT_MAX_DEPTH,
        .max_values = H2_PAL_JSON_DEFAULT_MAX_VALUES,
    };
    h2_pal_json_document_t *document = NULL;
    h2_pal_json_value_t *string = NULL;
    assert(h2_pal_json_document_create(
               fixture.api, &limits, &document) == H2_PAL_OK);
    assert(h2_pal_json_value_create_string(
               fixture.api, document, "abc", 3u, &string) == H2_PAL_OK);
    assert(h2_pal_json_document_set_root(
               fixture.api, document, string) == H2_PAL_OK);
    h2_pal_json_buffer_t buffer = {
        .data = (uint8_t *)(uintptr_t)1u,
        .len = 1u,
    };
    assert(h2_pal_json_document_serialize(
               fixture.api, document, &buffer) == H2_PAL_ERR_NO_SPACE);
    assert(buffer.data == NULL && buffer.len == 0u);
    assert(h2_pal_json_document_destroy(
               fixture.api, &document) == H2_PAL_OK);
    destroy_provider(factory, &fixture);
}

static void test_allocation_failures(
    const h2_pal_json_test_provider_t *factory) {
    static const char input[] =
        "{\"a\":[1,2,3],\"b\":{\"c\":\"value\"}}";
    bool observed_no_memory = false;
    bool observed_success = false;
    for (size_t fail_at = 0u; fail_at < 96u; ++fail_at) {
        provider_fixture_t fixture;
        init_provider_fixture(&fixture);
        fixture.allocator.fail_at = fail_at;
        h2_pal_result_t result = factory->create(
            &fixture.mem, &fixture.provider, &fixture.api);
        if (result == H2_PAL_ERR_NO_MEMORY) {
            observed_no_memory = true;
            assert(fixture.provider == NULL);
        } else {
            assert(result == H2_PAL_OK);
            h2_pal_json_document_t *document = NULL;
            result = h2_pal_json_document_parse(
                fixture.api, (const uint8_t *)input, strlen(input), NULL,
                &document);
            if (result == H2_PAL_ERR_NO_MEMORY) {
                observed_no_memory = true;
            } else {
                assert(result == H2_PAL_OK);
                observed_success = true;
            }
            if (document != NULL) {
                assert(h2_pal_json_document_destroy(
                           fixture.api, &document) == H2_PAL_OK);
            }
            assert(factory->destroy(&fixture.provider) == H2_PAL_OK);
        }
        assert(fixture.allocator.live_allocations == 0u);
        assert(fixture.allocator.live_bytes == 0u);
    }
    assert(observed_no_memory && observed_success);
}

static void test_mutation_allocation_failures(
    const h2_pal_json_test_provider_t *factory) {
    bool observed_no_memory = false;
    bool observed_success = false;
    for (size_t fail_at = 0u; fail_at < 128u; ++fail_at) {
        provider_fixture_t fixture;
        init_provider_fixture(&fixture);
        fixture.allocator.fail_at = fail_at;
        h2_pal_result_t result = factory->create(
            &fixture.mem, &fixture.provider, &fixture.api);
        h2_pal_json_document_t *document = NULL;
        h2_pal_json_value_t *root = NULL;
        h2_pal_json_value_t *string = NULL;
        h2_pal_json_buffer_t buffer = {0};
        if (result == H2_PAL_OK) {
            result = h2_pal_json_document_create(
                fixture.api, NULL, &document);
        }
        if (result == H2_PAL_OK) {
            result = h2_pal_json_value_create_object(
                fixture.api, document, &root);
        }
        if (result == H2_PAL_OK) {
            result = h2_pal_json_document_set_root(
                fixture.api, document, root);
        }
        if (result == H2_PAL_OK) {
            result = h2_pal_json_value_create_string(
                fixture.api, document, "value", 5u, &string);
        }
        if (result == H2_PAL_OK) {
            result = h2_pal_json_object_set(
                fixture.api, root, "key", 3u, string);
        }
        if (result == H2_PAL_OK) {
            result = h2_pal_json_document_serialize(
                fixture.api, document, &buffer);
        }
        if (result == H2_PAL_OK) {
            observed_success = true;
            assert(h2_pal_json_buffer_release(
                       fixture.api, &buffer) == H2_PAL_OK);
        } else {
            assert(result == H2_PAL_ERR_NO_MEMORY);
            observed_no_memory = true;
        }
        if (document != NULL) {
            assert(h2_pal_json_document_destroy(
                       fixture.api, &document) == H2_PAL_OK);
        }
        if (fixture.provider != NULL) {
            assert(factory->destroy(&fixture.provider) == H2_PAL_OK);
        }
        assert(fixture.allocator.live_allocations == 0u);
        assert(fixture.allocator.live_bytes == 0u);
    }
    assert(observed_no_memory && observed_success);
}

void h2_pal_json_run_conformance(
    const h2_pal_json_test_provider_t *factory,
    h2_pal_json_conformance_result_t *out_result) {
    assert(factory != NULL && factory->name != NULL);
    assert(factory->create != NULL && factory->destroy != NULL);
    assert(out_result != NULL);
    memset(out_result, 0, sizeof(*out_result));
    test_parse_and_query(factory, &out_result->peak_live_bytes);
    test_strict_input_and_limits(factory);
    test_mutation_and_ownership(factory);
    test_immutable_and_construction_validation(factory);
    test_mutation_limits(factory);
    test_output_limit(factory);
    test_allocation_failures(factory);
    test_mutation_allocation_failures(factory);
    printf("%s peak_live_bytes=%zu\n", factory->name, out_result->peak_live_bytes);
}
