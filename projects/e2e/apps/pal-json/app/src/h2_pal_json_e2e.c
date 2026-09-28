#include "h2_pal_json_e2e.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define REQUIRE(condition) do { if (!(condition)) { rc = H2_PAL_ERR_INVALID_STATE; goto done; } } while (0)
#define REQUIRE_RC(expression, expected) REQUIRE((expression) == (expected))
#define DESTROY(api, document) do { if ((document) != NULL && h2_pal_json_document_destroy((api), &(document)) != H2_PAL_OK) rc = H2_PAL_ERR_INVALID_STATE; } while (0)

static h2_pal_result_t test_parse_values(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    static const uint8_t input[] = "{\"s\":\"hi\",\"a\":[true,42.25,null],\"o\":{}}";
    h2_pal_result_t rc = H2_PAL_OK;
    h2_pal_json_document_t *doc = NULL;
    h2_pal_json_value_t *root = NULL, *array = NULL, *value = NULL;
    h2_pal_json_type_t type = H2_PAL_JSON_TYPE_INVALID;
    h2_pal_json_string_view_t key = {0}, view = {0};
    h2_pal_json_buffer_t buffer = {0};
    size_t size = 0;
    bool boolean = false;
    double number = 0;
    REQUIRE_RC(h2_pal_json_document_parse(api, input, sizeof(input) - 1, NULL, &doc), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_root(api, doc, &root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_type(api, root, &type), H2_PAL_OK);
    REQUIRE(type == H2_PAL_JSON_TYPE_OBJECT);
    REQUIRE_RC(h2_pal_json_object_size(api, root, &size), H2_PAL_OK);
    REQUIRE(size == 3);
    for (size_t i = 0; i < size; ++i) {
        REQUIRE_RC(h2_pal_json_object_entry(api, root, i, &key, &value), H2_PAL_OK);
        REQUIRE(key.data != NULL && key.len != 0 && value != NULL);
    }
    REQUIRE_RC(h2_pal_json_object_get(api, root, "s", 1, &value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_get_string(api, value, &view), H2_PAL_OK);
    REQUIRE(view.len == 2 && memcmp(view.data, "hi", 2) == 0);
    REQUIRE_RC(h2_pal_json_object_get(api, root, "a", 1, &array), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_size(api, array, &size), H2_PAL_OK);
    REQUIRE(size == 3);
    REQUIRE_RC(h2_pal_json_array_get(api, array, 0, &value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_get_boolean(api, value, &boolean), H2_PAL_OK);
    REQUIRE(boolean);
    REQUIRE_RC(h2_pal_json_array_get(api, array, 1, &value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_get_number(api, value, &number), H2_PAL_OK);
    REQUIRE(number == 42.25);
    REQUIRE_RC(h2_pal_json_array_get(api, array, 2, &value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_type(api, value, &type), H2_PAL_OK);
    REQUIRE(type == H2_PAL_JSON_TYPE_NULL);
    REQUIRE_RC(h2_pal_json_document_serialize(api, doc, &buffer), H2_PAL_OK);
    REQUIRE(buffer.data != NULL && buffer.len > 0 && buffer.data[buffer.len] == 0);
done:
    if (buffer.data && h2_pal_json_buffer_release(api, &buffer) != H2_PAL_OK) rc = H2_PAL_ERR_INVALID_STATE;
    DESTROY(api, doc);
    return rc;
}

static h2_pal_result_t test_string_spans(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    static const uint8_t input[] = {'"', 'x', '\\', 'u', 'D', '8', '3', 'D', '\\', 'u', 'D', 'E', '0', '0', '"'};
    static const uint8_t decoded[] = {'x', 0xf0, 0x9f, 0x98, 0x80};
    h2_pal_result_t rc = H2_PAL_OK;
    h2_pal_json_document_t *doc = NULL;
    h2_pal_json_value_t *root = NULL;
    h2_pal_json_string_view_t view = {0};
    REQUIRE_RC(h2_pal_json_document_parse(api, input, sizeof(input), NULL, &doc), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_root(api, doc, &root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_get_string(api, root, &view), H2_PAL_OK);
    REQUIRE(view.len == sizeof(decoded) && memcmp(view.data, decoded, sizeof(decoded)) == 0);
done:
    DESTROY(api, doc);
    return rc;
}

static h2_pal_result_t test_strict_profile(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    static const char *invalid[] = {
        "", "{", "[1,]", "{\"a\":1,\"a\":2}", "true false", "\xef\xbb\xbf{}",
        "\"\\u0000\"", "\"\\ud800\"", "1e999", "\"\\udc00\"", "\"\\q\"", "/*x*/{}"
    };
    static const uint8_t invalid_utf8[] = {'"', 0xc0, 0x80, '"'};
    h2_pal_json_document_t *doc = NULL;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        doc = (h2_pal_json_document_t *)(uintptr_t)1;
        if (h2_pal_json_document_parse(api, (const uint8_t *)invalid[i], strlen(invalid[i]), NULL, &doc) != H2_PAL_ERR_FORMAT || doc != NULL)
            return H2_PAL_ERR_INVALID_STATE;
    }
    doc = (h2_pal_json_document_t *)(uintptr_t)1;
    if (h2_pal_json_document_parse(api, invalid_utf8, sizeof(invalid_utf8), NULL, &doc) != H2_PAL_ERR_FORMAT || doc != NULL)
        return H2_PAL_ERR_INVALID_STATE;
    return H2_PAL_OK;
}

static h2_pal_result_t test_parse_limits(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    h2_pal_json_limits_t limits = {H2_PAL_JSON_DEFAULT_MAX_DOCUMENT_BYTES, 2, H2_PAL_JSON_DEFAULT_MAX_VALUES};
    h2_pal_json_document_t *doc = (h2_pal_json_document_t *)(uintptr_t)1;
    if (h2_pal_json_document_parse(api, (const uint8_t *)"[[[]]]", 6, &limits, &doc) != H2_PAL_ERR_NO_SPACE || doc)
        return H2_PAL_ERR_INVALID_STATE;
    limits.max_depth = H2_PAL_JSON_DEFAULT_MAX_DEPTH;
    limits.max_values = 2;
    if (h2_pal_json_document_parse(api, (const uint8_t *)"[1,2]", 5, &limits, &doc) != H2_PAL_ERR_NO_SPACE || doc)
        return H2_PAL_ERR_INVALID_STATE;
    limits.max_values = H2_PAL_JSON_DEFAULT_MAX_VALUES;
    limits.max_document_bytes = 2;
    if (h2_pal_json_document_parse(api, (const uint8_t *)"\"x\"", 3, &limits, &doc) != H2_PAL_ERR_NO_SPACE || doc)
        return H2_PAL_ERR_INVALID_STATE;
    limits.max_document_bytes = 0;
    if (h2_pal_json_document_parse(api, (const uint8_t *)"null", 4, &limits, &doc) != H2_PAL_ERR_INVALID_ARG || doc)
        return H2_PAL_ERR_INVALID_STATE;
    return H2_PAL_OK;
}

static h2_pal_result_t test_construct_roundtrip(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    h2_pal_result_t rc = H2_PAL_OK;
    h2_pal_json_document_t *doc = NULL, *parsed = NULL;
    h2_pal_json_value_t *root = NULL, *array = NULL, *value = NULL;
    h2_pal_json_buffer_t buffer = {0};
    REQUIRE_RC(h2_pal_json_document_create(api, NULL, &doc), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_object(api, doc, &root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_set_root(api, doc, root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_array(api, doc, &array), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_object_set(api, root, "values", 6, array), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_boolean(api, doc, true, &value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_append(api, array, value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_number(api, doc, 17.5, &value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_append(api, array, value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_null(api, doc, &value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_append(api, array, value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_string(api, doc, "ok", 2, &value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_object_set(api, root, "name", 4, value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_serialize(api, doc, &buffer), H2_PAL_OK);
    REQUIRE(buffer.data[buffer.len] == 0);
    REQUIRE_RC(h2_pal_json_document_parse(api, buffer.data, buffer.len, NULL, &parsed), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_root(api, parsed, &root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_object_get(api, root, "values", 6, &array), H2_PAL_OK);
    size_t size = 0;
    REQUIRE_RC(h2_pal_json_array_size(api, array, &size), H2_PAL_OK);
    REQUIRE(size == 3);
done:
    DESTROY(api, parsed);
    if (buffer.data && h2_pal_json_buffer_release(api, &buffer) != H2_PAL_OK) rc = H2_PAL_ERR_INVALID_STATE;
    DESTROY(api, doc);
    return rc;
}

static h2_pal_result_t test_lookup_absence(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    h2_pal_result_t rc = H2_PAL_OK;
    h2_pal_json_document_t *doc = NULL;
    h2_pal_json_value_t *root = (h2_pal_json_value_t *)(uintptr_t)1, *value = NULL;
    h2_pal_json_string_view_t key = {(const char *)(uintptr_t)1, 1};
    size_t size = 1;
    REQUIRE_RC(h2_pal_json_document_create(api, NULL, &doc), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_root(api, doc, &root), H2_PAL_ERR_NOT_FOUND);
    REQUIRE(root == NULL);
    REQUIRE_RC(h2_pal_json_value_create_object(api, doc, &root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_object_get(api, root, "missing", 7, &value), H2_PAL_ERR_NOT_FOUND);
    REQUIRE(value == NULL);
    REQUIRE_RC(h2_pal_json_object_entry(api, root, 0, &key, &value), H2_PAL_ERR_NOT_FOUND);
    REQUIRE(key.data == NULL && key.len == 0 && value == NULL);
    REQUIRE_RC(h2_pal_json_array_size(api, root, &size), H2_PAL_ERR_INVALID_STATE);
    REQUIRE(size == 0);
done:
    DESTROY(api, doc);
    return rc;
}

static h2_pal_result_t test_immutable_document(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    h2_pal_result_t rc = H2_PAL_OK;
    h2_pal_json_document_t *doc = NULL;
    h2_pal_json_value_t *root = NULL, *item = NULL;
    REQUIRE_RC(h2_pal_json_document_parse(api, (const uint8_t *)"[1]", 3, NULL, &doc), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_root(api, doc, &root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_get(api, root, 0, &item), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_append(api, root, item), H2_PAL_ERR_INVALID_STATE);
    REQUIRE_RC(h2_pal_json_document_set_root(api, doc, item), H2_PAL_ERR_INVALID_STATE);
done:
    DESTROY(api, doc);
    return rc;
}

static h2_pal_result_t test_ownership_and_cycles(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    h2_pal_result_t rc = H2_PAL_OK;
    h2_pal_json_document_t *doc = NULL, *other = NULL;
    h2_pal_json_value_t *array = NULL, *object = NULL, *value = NULL;
    h2_pal_json_value_t *cycle_object = NULL, *cycle_array = NULL;
    REQUIRE_RC(h2_pal_json_document_create(api, NULL, &doc), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_create(api, NULL, &other), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_array(api, doc, &array), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_set_root(api, doc, array), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_object(api, doc, &object), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_append(api, array, object), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_append(api, array, object), H2_PAL_ERR_INVALID_STATE);
    REQUIRE_RC(h2_pal_json_value_create_object(api, doc, &cycle_object), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_array(api, doc, &cycle_array), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_object_set(api, cycle_object, "child", 5, cycle_array), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_append(api, cycle_array, cycle_object), H2_PAL_ERR_INVALID_ARG);
    REQUIRE_RC(h2_pal_json_value_create_null(api, other, &value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_append(api, array, value), H2_PAL_ERR_INVALID_ARG);
done:
    DESTROY(api, other);
    DESTROY(api, doc);
    return rc;
}

static h2_pal_result_t test_replacement_stale_view(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    h2_pal_result_t rc = H2_PAL_OK;
    h2_pal_json_document_t *doc = NULL;
    h2_pal_json_value_t *root = NULL, *old = NULL, *new_value = NULL, *looked_up = NULL;
    h2_pal_json_string_view_t view = {0};
    REQUIRE_RC(h2_pal_json_document_create(api, NULL, &doc), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_object(api, doc, &root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_set_root(api, doc, root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_string(api, doc, "old", 3, &old), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_object_set(api, root, "name", 4, old), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_get_string(api, old, &view), H2_PAL_OK);
    REQUIRE(view.len == 3);
    REQUIRE_RC(h2_pal_json_value_create_string(api, doc, "new", 3, &new_value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_object_set(api, root, "name", 4, new_value), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_get_string(api, old, &view), H2_PAL_ERR_INVALID_STATE);
    REQUIRE(view.data == NULL && view.len == 0);
    REQUIRE_RC(h2_pal_json_object_get(api, root, "name", 4, &looked_up), H2_PAL_OK);
    REQUIRE(looked_up == new_value);
done:
    DESTROY(api, doc);
    return rc;
}

static h2_pal_result_t test_construction_rejection(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    h2_pal_result_t rc = H2_PAL_OK;
    h2_pal_json_document_t *doc = NULL;
    h2_pal_json_value_t *value = (h2_pal_json_value_t *)(uintptr_t)1;
    const char nul[] = {'a', 0, 'b'};
    static const uint8_t invalid[] = {0xc0, 0x80};
    REQUIRE_RC(h2_pal_json_document_create(api, NULL, &doc), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_string(api, doc, (const char *)invalid, sizeof(invalid), &value), H2_PAL_ERR_INVALID_ARG);
    REQUIRE(value == NULL);
    REQUIRE_RC(h2_pal_json_value_create_string(api, doc, nul, sizeof(nul), &value), H2_PAL_ERR_INVALID_ARG);
    REQUIRE(value == NULL);
    REQUIRE_RC(h2_pal_json_value_create_number(api, doc, INFINITY, &value), H2_PAL_ERR_INVALID_ARG);
    REQUIRE(value == NULL);
    REQUIRE_RC(h2_pal_json_value_create_string(api, doc, "x", SIZE_MAX, &value), H2_PAL_ERR_INVALID_ARG);
    REQUIRE(value == NULL);
done:
    DESTROY(api, doc);
    return rc;
}

static h2_pal_result_t test_mutation_limits(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    const h2_pal_json_limits_t limits = {H2_PAL_JSON_DEFAULT_MAX_DOCUMENT_BYTES, 2, 3};
    h2_pal_result_t rc = H2_PAL_OK;
    h2_pal_json_document_t *doc = NULL;
    h2_pal_json_value_t *root = NULL, *child = NULL, *excess = NULL;
    REQUIRE_RC(h2_pal_json_document_create(api, &limits, &doc), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_array(api, doc, &root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_set_root(api, doc, root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_array(api, doc, &child), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_append(api, root, child), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_null(api, doc, &excess), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_array_append(api, child, excess), H2_PAL_ERR_NO_SPACE);
    excess = (h2_pal_json_value_t *)(uintptr_t)1;
    REQUIRE_RC(h2_pal_json_value_create_null(api, doc, &excess), H2_PAL_ERR_NO_SPACE);
    REQUIRE(excess == NULL);
done:
    DESTROY(api, doc);
    return rc;
}

static h2_pal_result_t test_serialization_limit(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    const h2_pal_json_limits_t limits = {4, H2_PAL_JSON_DEFAULT_MAX_DEPTH, H2_PAL_JSON_DEFAULT_MAX_VALUES};
    h2_pal_result_t rc = H2_PAL_OK;
    h2_pal_json_document_t *doc = NULL;
    h2_pal_json_value_t *root = NULL;
    h2_pal_json_buffer_t buffer = {0};
    REQUIRE_RC(h2_pal_json_document_create(api, &limits, &doc), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_value_create_string(api, doc, "abc", 3, &root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_set_root(api, doc, root), H2_PAL_OK);
    REQUIRE_RC(h2_pal_json_document_serialize(api, doc, &buffer), H2_PAL_ERR_NO_SPACE);
    REQUIRE(buffer.data == NULL && buffer.len == 0);
done:
    if (buffer.data && h2_pal_json_buffer_release(api, &buffer) != H2_PAL_OK) rc = H2_PAL_ERR_INVALID_STATE;
    DESTROY(api, doc);
    return rc;
}

static h2_pal_result_t test_empty_cleanup(const h2_pal_json_api_t *api, const h2_pal_mem_api_t *unused_mem, h2_pal_json_e2e_probe_fn unused_probe) {
    (void)unused_mem; (void)unused_probe;
    h2_pal_json_document_t *doc = NULL;
    h2_pal_json_buffer_t buffer = {0};
    if (h2_pal_json_document_destroy(api, &doc) != H2_PAL_OK ||
        h2_pal_json_document_destroy(api, &doc) != H2_PAL_OK ||
        h2_pal_json_buffer_release(api, &buffer) != H2_PAL_OK ||
        h2_pal_json_buffer_release(api, &buffer) != H2_PAL_OK)
        return H2_PAL_ERR_INVALID_STATE;
    return H2_PAL_OK;
}

static h2_pal_result_t test_provider_live_output(
    const h2_pal_json_api_t *api, const h2_pal_mem_api_t *mem,
    h2_pal_json_e2e_probe_fn probe) {
    (void)api;
    return probe(mem, H2_PAL_JSON_E2E_PROBE_LIVE_OUTPUT);
}

static h2_pal_result_t test_allocator_failure_cleanup(
    const h2_pal_json_api_t *api, const h2_pal_mem_api_t *mem,
    h2_pal_json_e2e_probe_fn probe) {
    (void)api;
    return probe(mem, H2_PAL_JSON_E2E_PROBE_ALLOCATOR_FAILURE);
}

static int complete_api(const h2_pal_json_api_t *api) {
    return api && api->vtable &&
        api->vtable->document_parse && api->vtable->document_create &&
        api->vtable->document_destroy && api->vtable->document_root &&
        api->vtable->document_set_root && api->vtable->value_create_null &&
        api->vtable->value_create_boolean && api->vtable->value_create_number &&
        api->vtable->value_create_string && api->vtable->value_create_array &&
        api->vtable->value_create_object && api->vtable->value_type &&
        api->vtable->value_get_boolean && api->vtable->value_get_number &&
        api->vtable->value_get_string && api->vtable->array_size &&
        api->vtable->array_get && api->vtable->array_append &&
        api->vtable->object_get && api->vtable->object_size &&
        api->vtable->object_entry && api->vtable->object_set &&
        api->vtable->document_serialize && api->vtable->buffer_release;
}

h2_pal_result_t h2_pal_json_e2e_run(h2_runtime_t *runtime,
                                    const h2_pal_json_api_t *json,
                                    h2_pal_json_e2e_probe_fn probe,
                                    h2_pal_json_e2e_result_t *result) {
    if (!result) return H2_PAL_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    typedef h2_pal_result_t (*test_fn)(const h2_pal_json_api_t *, const h2_pal_mem_api_t *, h2_pal_json_e2e_probe_fn);
    static const struct { const char *id; test_fn run; } cases[] = {
#define H2_PAL_JSON_CASE(id, function) {id, function},
#include "h2_pal_json_cases.inc"
#undef H2_PAL_JSON_CASE
    };
    const int available = runtime && runtime->mem && probe && complete_api(json);
    for (size_t i = 0; i < H2_PAL_JSON_E2E_CASE_COUNT; ++i) {
        result->cases[i].id = cases[i].id;
        if (!available) {
            result->cases[i].status = H2_PAL_JSON_E2E_BLOCKED;
            result->cases[i].result = H2_PAL_ERR_UNSUPPORTED;
            ++result->blocked;
        } else {
            h2_pal_result_t rc = cases[i].run(json, runtime->mem, probe);
            result->cases[i].result = rc;
            result->cases[i].status = rc == H2_PAL_OK ? H2_PAL_JSON_E2E_PASS : H2_PAL_JSON_E2E_FAIL;
            if (rc == H2_PAL_OK) ++result->passed; else ++result->failed;
        }
    }
    result->complete = result->passed + result->failed + result->blocked == H2_PAL_JSON_E2E_CASE_COUNT;
    result->qualified = result->complete && result->passed == H2_PAL_JSON_E2E_CASE_COUNT;
    return result->qualified ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
