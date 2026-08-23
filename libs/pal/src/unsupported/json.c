#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_json_document_parse(
    void *user, const uint8_t *data, size_t len,
    const h2_pal_json_limits_t *limits,
    h2_pal_json_document_t **out_document) {
    (void)user; (void)data; (void)len; (void)limits;
    *out_document = NULL;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_document_create(
    void *user, const h2_pal_json_limits_t *limits,
    h2_pal_json_document_t **out_document) {
    (void)user; (void)limits;
    *out_document = NULL;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_document_destroy(
    void *user, h2_pal_json_document_t **document) {
    (void)user;
    *document = NULL;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_document_root(
    void *user, h2_pal_json_document_t *document,
    h2_pal_json_value_t **out_value) {
    (void)user; (void)document;
    *out_value = NULL;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_document_set_root(
    void *user, h2_pal_json_document_t *document,
    h2_pal_json_value_t *value) {
    (void)user; (void)document; (void)value;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_value_create(
    void *user, h2_pal_json_document_t *document,
    h2_pal_json_value_t **out_value) {
    (void)user; (void)document;
    *out_value = NULL;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_value_create_boolean(
    void *user, h2_pal_json_document_t *document, bool value,
    h2_pal_json_value_t **out_value) {
    (void)value;
    return unsupported_json_value_create(user, document, out_value);
}
static h2_pal_result_t unsupported_json_value_create_number(
    void *user, h2_pal_json_document_t *document, double value,
    h2_pal_json_value_t **out_value) {
    (void)value;
    return unsupported_json_value_create(user, document, out_value);
}
static h2_pal_result_t unsupported_json_value_create_string(
    void *user, h2_pal_json_document_t *document, const char *data,
    size_t len, h2_pal_json_value_t **out_value) {
    (void)data; (void)len;
    return unsupported_json_value_create(user, document, out_value);
}
static h2_pal_result_t unsupported_json_value_type(
    void *user, const h2_pal_json_value_t *value,
    h2_pal_json_type_t *out_type) {
    (void)user; (void)value;
    *out_type = H2_PAL_JSON_TYPE_INVALID;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_value_get_boolean(
    void *user, const h2_pal_json_value_t *value, bool *out_value) {
    (void)user; (void)value;
    *out_value = false;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_value_get_number(
    void *user, const h2_pal_json_value_t *value, double *out_value) {
    (void)user; (void)value;
    *out_value = 0.0;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_value_get_string(
    void *user, const h2_pal_json_value_t *value,
    h2_pal_json_string_view_t *out_value) {
    (void)user; (void)value;
    out_value->data = NULL;
    out_value->len = 0u;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_array_size(
    void *user, const h2_pal_json_value_t *array, size_t *out_size) {
    (void)user; (void)array;
    *out_size = 0u;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_array_get(
    void *user, const h2_pal_json_value_t *array, size_t index,
    h2_pal_json_value_t **out_value) {
    (void)user; (void)array; (void)index;
    *out_value = NULL;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_array_append(
    void *user, h2_pal_json_value_t *array, h2_pal_json_value_t *value) {
    (void)user; (void)array; (void)value;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_object_get(
    void *user, const h2_pal_json_value_t *object, const char *key,
    size_t key_len, h2_pal_json_value_t **out_value) {
    (void)user; (void)object; (void)key; (void)key_len;
    *out_value = NULL;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_object_size(
    void *user, const h2_pal_json_value_t *object, size_t *out_size) {
    (void)user; (void)object;
    *out_size = 0u;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_object_entry(
    void *user, const h2_pal_json_value_t *object, size_t index,
    h2_pal_json_string_view_t *out_key,
    h2_pal_json_value_t **out_value) {
    (void)user; (void)object; (void)index;
    out_key->data = NULL;
    out_key->len = 0u;
    *out_value = NULL;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_object_set(
    void *user, h2_pal_json_value_t *object, const char *key,
    size_t key_len, h2_pal_json_value_t *value) {
    (void)user; (void)object; (void)key; (void)key_len; (void)value;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_document_serialize(
    void *user, const h2_pal_json_document_t *document,
    h2_pal_json_buffer_t *out_buffer) {
    (void)user; (void)document;
    out_buffer->data = NULL;
    out_buffer->len = 0u;
    return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t unsupported_json_buffer_release(
    void *user, h2_pal_json_buffer_t *buffer) {
    (void)user;
    buffer->data = NULL;
    buffer->len = 0u;
    return H2_PAL_ERR_UNSUPPORTED;
}
static const h2_pal_json_vtable_t unsupported_json_vtable = {
    .document_parse = unsupported_json_document_parse,
    .document_create = unsupported_json_document_create,
    .document_destroy = unsupported_json_document_destroy,
    .document_root = unsupported_json_document_root,
    .document_set_root = unsupported_json_document_set_root,
    .value_create_null = unsupported_json_value_create,
    .value_create_boolean = unsupported_json_value_create_boolean,
    .value_create_number = unsupported_json_value_create_number,
    .value_create_string = unsupported_json_value_create_string,
    .value_create_array = unsupported_json_value_create,
    .value_create_object = unsupported_json_value_create,
    .value_type = unsupported_json_value_type,
    .value_get_boolean = unsupported_json_value_get_boolean,
    .value_get_number = unsupported_json_value_get_number,
    .value_get_string = unsupported_json_value_get_string,
    .array_size = unsupported_json_array_size,
    .array_get = unsupported_json_array_get,
    .array_append = unsupported_json_array_append,
    .object_get = unsupported_json_object_get,
    .object_size = unsupported_json_object_size,
    .object_entry = unsupported_json_object_entry,
    .object_set = unsupported_json_object_set,
    .document_serialize = unsupported_json_document_serialize,
    .buffer_release = unsupported_json_buffer_release,
};
static const h2_pal_json_api_t unsupported_json_api = {
    .user = NULL,
    .vtable = &unsupported_json_vtable,
};
const h2_pal_json_api_t *h2_pal_unsupported_json_api(void) {
    return &unsupported_json_api;
}
