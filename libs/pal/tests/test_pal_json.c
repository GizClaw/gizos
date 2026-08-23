#include "h2_pal.h"

#include <assert.h>
#include <stdint.h>

static h2_pal_result_t dirty_document_create(
    void *user,
    const h2_pal_json_limits_t *limits,
    h2_pal_json_document_t **out_document) {
    (void)user;
    (void)limits;
    *out_document = (h2_pal_json_document_t *)(uintptr_t)1u;
    return H2_PAL_ERR_NO_MEMORY;
}

static h2_pal_result_t dirty_value_type(
    void *user,
    const h2_pal_json_value_t *value,
    h2_pal_json_type_t *out_type) {
    (void)user;
    (void)value;
    *out_type = H2_PAL_JSON_TYPE_OBJECT;
    return H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t dirty_value_get_boolean(
    void *user,
    const h2_pal_json_value_t *value,
    bool *out_value) {
    (void)user;
    (void)value;
    *out_value = true;
    return H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t dirty_value_get_number(
    void *user,
    const h2_pal_json_value_t *value,
    double *out_value) {
    (void)user;
    (void)value;
    *out_value = 1.0;
    return H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t dirty_value_get_string(
    void *user,
    const h2_pal_json_value_t *value,
    h2_pal_json_string_view_t *out_value) {
    (void)user;
    (void)value;
    out_value->data = (const char *)(uintptr_t)1u;
    out_value->len = 1u;
    return H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t dirty_array_size(
    void *user,
    const h2_pal_json_value_t *value,
    size_t *out_size) {
    (void)user;
    (void)value;
    *out_size = 1u;
    return H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t dirty_object_size(
    void *user,
    const h2_pal_json_value_t *value,
    size_t *out_size) {
    (void)user;
    (void)value;
    *out_size = 1u;
    return H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t dirty_object_entry(
    void *user,
    const h2_pal_json_value_t *value,
    size_t index,
    h2_pal_json_string_view_t *out_key,
    h2_pal_json_value_t **out_value) {
    (void)user;
    (void)value;
    (void)index;
    out_key->data = (const char *)(uintptr_t)1u;
    out_key->len = 1u;
    *out_value = (h2_pal_json_value_t *)(uintptr_t)1u;
    return H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t dirty_document_serialize(
    void *user,
    const h2_pal_json_document_t *document,
    h2_pal_json_buffer_t *out_buffer) {
    (void)user;
    (void)document;
    out_buffer->data = (uint8_t *)(uintptr_t)1u;
    out_buffer->len = 1u;
    return H2_PAL_ERR_NO_MEMORY;
}

static void test_backend_failure_outputs_are_normalized(void) {
    static const h2_pal_json_vtable_t dirty_vtable = {
        .document_create = dirty_document_create,
        .value_type = dirty_value_type,
        .value_get_boolean = dirty_value_get_boolean,
        .value_get_number = dirty_value_get_number,
        .value_get_string = dirty_value_get_string,
        .array_size = dirty_array_size,
        .object_size = dirty_object_size,
        .object_entry = dirty_object_entry,
        .document_serialize = dirty_document_serialize,
    };
    const h2_pal_json_api_t api = {
        .user = NULL,
        .vtable = &dirty_vtable,
    };
    h2_pal_json_document_t *document = NULL;
    assert(h2_pal_json_document_create(&api, NULL, &document) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(document == NULL);

    const h2_pal_json_value_t *value =
        (const h2_pal_json_value_t *)(uintptr_t)1u;
    h2_pal_json_type_t type = H2_PAL_JSON_TYPE_OBJECT;
    assert(h2_pal_json_value_type(&api, value, &type) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(type == H2_PAL_JSON_TYPE_INVALID);
    bool boolean = true;
    assert(h2_pal_json_value_get_boolean(&api, value, &boolean) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(!boolean);
    double number = 1.0;
    assert(h2_pal_json_value_get_number(&api, value, &number) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(number == 0.0);
    h2_pal_json_string_view_t string = {
        .data = (const char *)(uintptr_t)1u,
        .len = 1u,
    };
    assert(h2_pal_json_value_get_string(&api, value, &string) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(string.data == NULL && string.len == 0u);
    size_t size = 1u;
    assert(h2_pal_json_array_size(&api, value, &size) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(size == 0u);
    size = 1u;
    assert(h2_pal_json_object_size(&api, value, &size) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(size == 0u);
    h2_pal_json_string_view_t key = {
        .data = (const char *)(uintptr_t)1u,
        .len = 1u,
    };
    h2_pal_json_value_t *entry = (h2_pal_json_value_t *)(uintptr_t)1u;
    assert(h2_pal_json_object_entry(&api, value, 0u, &key, &entry) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(key.data == NULL && key.len == 0u && entry == NULL);

    h2_pal_json_buffer_t buffer = {
        .data = (uint8_t *)(uintptr_t)1u,
        .len = 1u,
    };
    assert(h2_pal_json_document_serialize(
               &api, (const h2_pal_json_document_t *)(uintptr_t)1u,
               &buffer) == H2_PAL_ERR_NO_MEMORY);
    assert(buffer.data == NULL && buffer.len == 0u);
}

static void test_unsupported_surface(void) {
    const h2_pal_json_api_t *api = h2_pal_unsupported_json_api();
    h2_pal_json_document_t *document =
        (h2_pal_json_document_t *)(uintptr_t)1u;
    h2_pal_json_value_t *value = (h2_pal_json_value_t *)(uintptr_t)2u;
    h2_pal_json_value_t *out_value = (h2_pal_json_value_t *)(uintptr_t)3u;
    assert(h2_pal_json_document_root(api, document, &out_value) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(out_value == NULL);
    assert(h2_pal_json_document_set_root(api, document, value) ==
           H2_PAL_ERR_UNSUPPORTED);

#define CHECK_CREATE(call)                         \
    do {                                           \
        out_value = (void *)(uintptr_t)3u;         \
        assert((call) == H2_PAL_ERR_UNSUPPORTED);  \
        assert(out_value == NULL);                 \
    } while (0)
    CHECK_CREATE(h2_pal_json_value_create_null(api, document, &out_value));
    CHECK_CREATE(h2_pal_json_value_create_boolean(
        api, document, true, &out_value));
    CHECK_CREATE(h2_pal_json_value_create_number(
        api, document, 1.0, &out_value));
    CHECK_CREATE(h2_pal_json_value_create_string(
        api, document, "x", 1u, &out_value));
    CHECK_CREATE(h2_pal_json_value_create_array(api, document, &out_value));
    CHECK_CREATE(h2_pal_json_value_create_object(api, document, &out_value));
#undef CHECK_CREATE

    h2_pal_json_type_t type = H2_PAL_JSON_TYPE_OBJECT;
    assert(h2_pal_json_value_type(api, value, &type) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(type == H2_PAL_JSON_TYPE_INVALID);
    bool boolean = true;
    assert(h2_pal_json_value_get_boolean(api, value, &boolean) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(!boolean);
    double number = 1.0;
    assert(h2_pal_json_value_get_number(api, value, &number) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(number == 0.0);
    h2_pal_json_string_view_t string = {
        .data = (const char *)(uintptr_t)1u,
        .len = 1u,
    };
    assert(h2_pal_json_value_get_string(api, value, &string) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(string.data == NULL && string.len == 0u);
    size_t size = 1u;
    assert(h2_pal_json_array_size(api, value, &size) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(size == 0u);
    out_value = (h2_pal_json_value_t *)(uintptr_t)3u;
    assert(h2_pal_json_array_get(api, value, 0u, &out_value) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(out_value == NULL);
    assert(h2_pal_json_array_append(api, value, value) ==
           H2_PAL_ERR_UNSUPPORTED);
    out_value = (h2_pal_json_value_t *)(uintptr_t)3u;
    assert(h2_pal_json_object_get(api, value, "x", 1u, &out_value) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(out_value == NULL);
    size = 1u;
    assert(h2_pal_json_object_size(api, value, &size) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(size == 0u);
    h2_pal_json_string_view_t key = {
        .data = (const char *)(uintptr_t)1u,
        .len = 1u,
    };
    out_value = (h2_pal_json_value_t *)(uintptr_t)3u;
    assert(h2_pal_json_object_entry(
               api, value, 0u, &key, &out_value) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(key.data == NULL && key.len == 0u && out_value == NULL);
    assert(h2_pal_json_object_set(api, value, "x", 1u, value) ==
           H2_PAL_ERR_UNSUPPORTED);
    h2_pal_json_buffer_t buffer = {
        .data = (uint8_t *)(uintptr_t)1u,
        .len = 1u,
    };
    assert(h2_pal_json_document_serialize(api, document, &buffer) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(buffer.data == NULL && buffer.len == 0u);

    h2_pal_json_document_t *null_document = NULL;
    assert(h2_pal_json_document_destroy(api, &null_document) == H2_PAL_OK);
    h2_pal_json_document_t *unsupported_document =
        (h2_pal_json_document_t *)(uintptr_t)1u;
    assert(h2_pal_json_document_destroy(api, &unsupported_document) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(unsupported_document == NULL);
    buffer.data = NULL;
    buffer.len = 0u;
    assert(h2_pal_json_buffer_release(api, &buffer) == H2_PAL_OK);
    buffer.data = (uint8_t *)(uintptr_t)1u;
    buffer.len = 1u;
    assert(h2_pal_json_buffer_release(api, &buffer) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(buffer.data == NULL && buffer.len == 0u);
}

int main(void) {
    static const h2_pal_json_vtable_t empty_vtable = {0};
    const h2_pal_json_api_t missing_vtable = {
        .user = NULL,
        .vtable = NULL,
    };
    const h2_pal_json_api_t incomplete_api = {
        .user = NULL,
        .vtable = &empty_vtable,
    };
    h2_pal_json_document_t *document =
        (h2_pal_json_document_t *)(uintptr_t)1u;
    assert(h2_pal_json_document_create(NULL, NULL, &document) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(document == NULL);
    document = (h2_pal_json_document_t *)(uintptr_t)1u;
    assert(h2_pal_json_document_create(&missing_vtable, NULL, &document) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(document == NULL);
    document = (h2_pal_json_document_t *)(uintptr_t)1u;
    assert(h2_pal_json_document_create(&incomplete_api, NULL, &document) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(document == NULL);

    document = (h2_pal_json_document_t *)(uintptr_t)1u;
    assert(h2_pal_json_document_parse(
               h2_pal_unsupported_json_api(), (const uint8_t *)"null", 4u,
               NULL, &document) == H2_PAL_ERR_UNSUPPORTED);
    assert(document == NULL);

    h2_pal_json_value_t *value = (h2_pal_json_value_t *)(uintptr_t)1u;
    assert(h2_pal_json_document_root(NULL, NULL, &value) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(value == NULL);
    h2_pal_json_type_t type = H2_PAL_JSON_TYPE_OBJECT;
    assert(h2_pal_json_value_type(NULL, NULL, &type) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(type == H2_PAL_JSON_TYPE_INVALID);
    h2_pal_json_string_view_t string = {
        .data = (const char *)(uintptr_t)1u,
        .len = 1u,
    };
    assert(h2_pal_json_value_get_string(NULL, NULL, &string) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(string.data == NULL && string.len == 0u);
    h2_pal_json_buffer_t buffer = {
        .data = NULL,
        .len = 1u,
    };
    assert(h2_pal_json_buffer_release(NULL, &buffer) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(buffer.data == NULL && buffer.len == 0u);
    test_backend_failure_outputs_are_normalized();
    test_unsupported_surface();
    return 0;
}
