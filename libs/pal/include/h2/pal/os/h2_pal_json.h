#ifndef H2_PAL_JSON_H
#define H2_PAL_JSON_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @file
 * @brief Strict, bounded, backend-neutral JSON PAL capability.
 *
 * Input must be one complete RFC 8259 UTF-8 document. Providers reject a BOM,
 * comments, trailing commas or content, duplicate object keys, invalid UTF-8,
 * invalid escapes or surrogate pairs, decoded U+0000, and numbers that do not
 * produce a finite `double`. Serialization is compact UTF-8 and appends a NUL
 * terminator that is not included in the reported length. Whitespace and
 * object-member order are not observable contracts.
 *
 * A parsed document is immutable; a created document is mutable. A document
 * owns all values created through it, including detached values. Successful
 * root/set/append operations attach a detached value. Repeated attachment is
 * invalid state; cross-document attachment and cycles are invalid arguments.
 * Replacing an object member invalidates outstanding handles and views into the
 * replaced subtree. Documents own borrowed value handles and string views;
 * serialized buffers remain owned by their originating provider until release.
 *
 * Provider instances are not internally synchronized. Callers must serialize
 * all operations using one provider or any of its documents and buffers.
 * Required outputs are cleared before validation. A NULL API returns
 * `H2_PAL_ERR_UNSUPPORTED`; a non-NULL incomplete API returns
 * `H2_PAL_ERR_INVALID_ARG`. Null document destruction and empty buffer release
 * are successful idempotent no-ops.
 */

/** Maximum default input or serialized JSON bytes, excluding the terminator. */
#define H2_PAL_JSON_DEFAULT_MAX_DOCUMENT_BYTES ((size_t)64u * 1024u)
/** Maximum default nesting depth, with the root at depth one. */
#define H2_PAL_JSON_DEFAULT_MAX_DEPTH ((size_t)32u)
/** Maximum default number of values; object keys are not values. */
#define H2_PAL_JSON_DEFAULT_MAX_VALUES ((size_t)4096u)

/** Document-owned opaque JSON document handle. */
typedef struct h2_pal_json_document h2_pal_json_document_t;
/** Borrowed opaque JSON value handle. */
typedef struct h2_pal_json_value h2_pal_json_value_t;

/** Observable JSON value type. */
typedef enum h2_pal_json_type {
    H2_PAL_JSON_TYPE_INVALID = 0,
    H2_PAL_JSON_TYPE_NULL,
    H2_PAL_JSON_TYPE_BOOLEAN,
    H2_PAL_JSON_TYPE_NUMBER,
    H2_PAL_JSON_TYPE_STRING,
    H2_PAL_JSON_TYPE_ARRAY,
    H2_PAL_JSON_TYPE_OBJECT,
} h2_pal_json_type_t;

/**
 * @brief Bounded document limits.
 *
 * A NULL limits pointer selects all defaults. Every explicit field must be in
 * `1..default`; zero or an attempted increase is invalid. Byte limits apply to
 * both input and output. Depth and value limits apply to parse and construction
 * without partially attaching a value when an operation exceeds them.
 */
typedef struct h2_pal_json_limits {
    size_t max_document_bytes;
    size_t max_depth;
    size_t max_values;
} h2_pal_json_limits_t;

/**
 * @brief Borrowed decoded UTF-8 string view.
 *
 * The length excludes any terminator and the data need not be NUL terminated.
 * The view remains valid until document destruction or replacement of its
 * owning subtree.
 */
typedef struct h2_pal_json_string_view {
    const char *data;
    size_t len;
} h2_pal_json_string_view_t;

/**
 * @brief Provider-owned compact JSON bytes.
 *
 * On successful serialization, `data[len]` is a non-counted NUL terminator.
 * Release the buffer through the same provider API that created it.
 */
typedef struct h2_pal_json_buffer {
    uint8_t *data;
    size_t len;
} h2_pal_json_buffer_t;

/**
 * @brief JSON provider dispatch table.
 *
 * `document_parse` borrows its byte span synchronously and returns an immutable
 * document. `document_create` returns an empty mutable document.
 * `document_destroy` releases every owned value and clears the caller handle.
 * `document_root` returns a borrowed root or `H2_PAL_ERR_NOT_FOUND` for an empty
 * mutable document. `document_set_root`, `array_append`, and `object_set`
 * attach detached values; `object_set` replaces an exact length-and-byte key.
 * Scalar accessors require the matching type. Array/object lookup returns
 * `H2_PAL_ERR_NOT_FOUND` when absent. Object iteration exposes borrowed key
 * views and value handles by index; member order is provider-specific and an
 * index is valid only while the object subtree is not mutated.
 * `document_serialize` returns a provider-owned buffer and `buffer_release`
 * frees and clears it.
 *
 * Stable errors are: invalid argument for invalid construction input, limits,
 * cross-document values, cycles, or an incomplete API; format for rejected
 * input syntax/profile; no space for byte/depth/value limits; no memory for
 * allocation failure; not found for absent roots/items/members; invalid state
 * for wrong types, immutable mutation, repeated attachment, stale handles, or
 * live-output provider destruction; and unsupported for a missing capability.
 */
typedef struct h2_pal_json_vtable {
    h2_pal_result_t (*document_parse)(
        void *user,
        const uint8_t *data,
        size_t len,
        const h2_pal_json_limits_t *limits,
        h2_pal_json_document_t **out_document);
    h2_pal_result_t (*document_create)(
        void *user,
        const h2_pal_json_limits_t *limits,
        h2_pal_json_document_t **out_document);
    h2_pal_result_t (*document_destroy)(
        void *user,
        h2_pal_json_document_t **document);
    h2_pal_result_t (*document_root)(
        void *user,
        h2_pal_json_document_t *document,
        h2_pal_json_value_t **out_value);
    h2_pal_result_t (*document_set_root)(
        void *user,
        h2_pal_json_document_t *document,
        h2_pal_json_value_t *value);
    h2_pal_result_t (*value_create_null)(
        void *user,
        h2_pal_json_document_t *document,
        h2_pal_json_value_t **out_value);
    h2_pal_result_t (*value_create_boolean)(
        void *user,
        h2_pal_json_document_t *document,
        bool value,
        h2_pal_json_value_t **out_value);
    h2_pal_result_t (*value_create_number)(
        void *user,
        h2_pal_json_document_t *document,
        double value,
        h2_pal_json_value_t **out_value);
    h2_pal_result_t (*value_create_string)(
        void *user,
        h2_pal_json_document_t *document,
        const char *data,
        size_t len,
        h2_pal_json_value_t **out_value);
    h2_pal_result_t (*value_create_array)(
        void *user,
        h2_pal_json_document_t *document,
        h2_pal_json_value_t **out_value);
    h2_pal_result_t (*value_create_object)(
        void *user,
        h2_pal_json_document_t *document,
        h2_pal_json_value_t **out_value);
    h2_pal_result_t (*value_type)(
        void *user,
        const h2_pal_json_value_t *value,
        h2_pal_json_type_t *out_type);
    h2_pal_result_t (*value_get_boolean)(
        void *user,
        const h2_pal_json_value_t *value,
        bool *out_value);
    h2_pal_result_t (*value_get_number)(
        void *user,
        const h2_pal_json_value_t *value,
        double *out_value);
    h2_pal_result_t (*value_get_string)(
        void *user,
        const h2_pal_json_value_t *value,
        h2_pal_json_string_view_t *out_value);
    h2_pal_result_t (*array_size)(
        void *user,
        const h2_pal_json_value_t *array,
        size_t *out_size);
    h2_pal_result_t (*array_get)(
        void *user,
        const h2_pal_json_value_t *array,
        size_t index,
        h2_pal_json_value_t **out_value);
    h2_pal_result_t (*array_append)(
        void *user,
        h2_pal_json_value_t *array,
        h2_pal_json_value_t *value);
    h2_pal_result_t (*object_get)(
        void *user,
        const h2_pal_json_value_t *object,
        const char *key,
        size_t key_len,
        h2_pal_json_value_t **out_value);
    h2_pal_result_t (*object_size)(
        void *user,
        const h2_pal_json_value_t *object,
        size_t *out_size);
    h2_pal_result_t (*object_entry)(
        void *user,
        const h2_pal_json_value_t *object,
        size_t index,
        h2_pal_json_string_view_t *out_key,
        h2_pal_json_value_t **out_value);
    h2_pal_result_t (*object_set)(
        void *user,
        h2_pal_json_value_t *object,
        const char *key,
        size_t key_len,
        h2_pal_json_value_t *value);
    h2_pal_result_t (*document_serialize)(
        void *user,
        const h2_pal_json_document_t *document,
        h2_pal_json_buffer_t *out_buffer);
    h2_pal_result_t (*buffer_release)(
        void *user,
        h2_pal_json_buffer_t *buffer);
} h2_pal_json_vtable_t;

/** Borrowed JSON capability object in the canonical PAL `user + vtable` shape. */
typedef struct h2_pal_json_api {
    void *user;
    const h2_pal_json_vtable_t *vtable;
} h2_pal_json_api_t;

static inline h2_pal_result_t h2_pal_json_document_parse(
    const h2_pal_json_api_t *api,
    const uint8_t *data,
    size_t len,
    const h2_pal_json_limits_t *limits,
    h2_pal_json_document_t **out_document) {
    if (out_document == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_document = NULL;
    if (data == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->document_parse == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result = api->vtable->document_parse(
        api->user, data, len, limits, out_document);
    if (result != H2_PAL_OK) {
        *out_document = NULL;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_document_create(
    const h2_pal_json_api_t *api,
    const h2_pal_json_limits_t *limits,
    h2_pal_json_document_t **out_document) {
    if (out_document == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_document = NULL;
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->document_create == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result =
        api->vtable->document_create(api->user, limits, out_document);
    if (result != H2_PAL_OK) {
        *out_document = NULL;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_document_destroy(
    const h2_pal_json_api_t *api,
    h2_pal_json_document_t **document) {
    if (document == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (*document == NULL) {
        return H2_PAL_OK;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->document_destroy == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->document_destroy(api->user, document);
}

static inline h2_pal_result_t h2_pal_json_document_root(
    const h2_pal_json_api_t *api,
    h2_pal_json_document_t *document,
    h2_pal_json_value_t **out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = NULL;
    if (document == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->document_root == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result =
        api->vtable->document_root(api->user, document, out_value);
    if (result != H2_PAL_OK) {
        *out_value = NULL;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_document_set_root(
    const h2_pal_json_api_t *api,
    h2_pal_json_document_t *document,
    h2_pal_json_value_t *value) {
    if (document == NULL || value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->document_set_root == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->document_set_root(api->user, document, value);
}

#define H2_PAL_JSON_CREATE_WRAPPER(name)                                      \
    static inline h2_pal_result_t h2_pal_json_##name(                         \
        const h2_pal_json_api_t *api,                                         \
        h2_pal_json_document_t *document,                                     \
        h2_pal_json_value_t **out_value) {                                    \
        if (out_value == NULL) {                                              \
            return H2_PAL_ERR_INVALID_ARG;                                    \
        }                                                                     \
        *out_value = NULL;                                                    \
        if (document == NULL) {                                               \
            return H2_PAL_ERR_INVALID_ARG;                                    \
        }                                                                     \
        if (api == NULL) {                                                    \
            return H2_PAL_ERR_UNSUPPORTED;                                    \
        }                                                                     \
        if (api->vtable == NULL || api->vtable->name == NULL) {              \
            return H2_PAL_ERR_INVALID_ARG;                                    \
        }                                                                     \
        const h2_pal_result_t result =                                      \
            api->vtable->name(api->user, document, out_value);              \
        if (result != H2_PAL_OK) {                                          \
            *out_value = NULL;                                              \
        }                                                                   \
        return result;                                                      \
    }

H2_PAL_JSON_CREATE_WRAPPER(value_create_null)
H2_PAL_JSON_CREATE_WRAPPER(value_create_array)
H2_PAL_JSON_CREATE_WRAPPER(value_create_object)

#undef H2_PAL_JSON_CREATE_WRAPPER

static inline h2_pal_result_t h2_pal_json_value_create_boolean(
    const h2_pal_json_api_t *api,
    h2_pal_json_document_t *document,
    bool value,
    h2_pal_json_value_t **out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = NULL;
    if (document == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->value_create_boolean == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result = api->vtable->value_create_boolean(
        api->user, document, value, out_value);
    if (result != H2_PAL_OK) {
        *out_value = NULL;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_value_create_number(
    const h2_pal_json_api_t *api,
    h2_pal_json_document_t *document,
    double value,
    h2_pal_json_value_t **out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = NULL;
    if (document == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->value_create_number == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result = api->vtable->value_create_number(
        api->user, document, value, out_value);
    if (result != H2_PAL_OK) {
        *out_value = NULL;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_value_create_string(
    const h2_pal_json_api_t *api,
    h2_pal_json_document_t *document,
    const char *data,
    size_t len,
    h2_pal_json_value_t **out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = NULL;
    if (document == NULL || len == SIZE_MAX ||
        (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->value_create_string == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result = api->vtable->value_create_string(
        api->user, document, data, len, out_value);
    if (result != H2_PAL_OK) {
        *out_value = NULL;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_value_type(
    const h2_pal_json_api_t *api,
    const h2_pal_json_value_t *value,
    h2_pal_json_type_t *out_type) {
    if (out_type == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_type = H2_PAL_JSON_TYPE_INVALID;
    if (value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->value_type == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result =
        api->vtable->value_type(api->user, value, out_type);
    if (result != H2_PAL_OK) {
        *out_type = H2_PAL_JSON_TYPE_INVALID;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_value_get_boolean(
    const h2_pal_json_api_t *api,
    const h2_pal_json_value_t *value,
    bool *out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = false;
    if (value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->value_get_boolean == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result =
        api->vtable->value_get_boolean(api->user, value, out_value);
    if (result != H2_PAL_OK) {
        *out_value = false;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_value_get_number(
    const h2_pal_json_api_t *api,
    const h2_pal_json_value_t *value,
    double *out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = 0.0;
    if (value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->value_get_number == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result =
        api->vtable->value_get_number(api->user, value, out_value);
    if (result != H2_PAL_OK) {
        *out_value = 0.0;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_value_get_string(
    const h2_pal_json_api_t *api,
    const h2_pal_json_value_t *value,
    h2_pal_json_string_view_t *out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_value->data = NULL;
    out_value->len = 0u;
    if (value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->value_get_string == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result =
        api->vtable->value_get_string(api->user, value, out_value);
    if (result != H2_PAL_OK) {
        out_value->data = NULL;
        out_value->len = 0u;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_array_size(
    const h2_pal_json_api_t *api,
    const h2_pal_json_value_t *array,
    size_t *out_size) {
    if (out_size == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_size = 0u;
    if (array == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->array_size == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result =
        api->vtable->array_size(api->user, array, out_size);
    if (result != H2_PAL_OK) {
        *out_size = 0u;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_array_get(
    const h2_pal_json_api_t *api,
    const h2_pal_json_value_t *array,
    size_t index,
    h2_pal_json_value_t **out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = NULL;
    if (array == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->array_get == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result =
        api->vtable->array_get(api->user, array, index, out_value);
    if (result != H2_PAL_OK) {
        *out_value = NULL;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_array_append(
    const h2_pal_json_api_t *api,
    h2_pal_json_value_t *array,
    h2_pal_json_value_t *value) {
    if (array == NULL || value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->array_append == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->array_append(api->user, array, value);
}

static inline h2_pal_result_t h2_pal_json_object_get(
    const h2_pal_json_api_t *api,
    const h2_pal_json_value_t *object,
    const char *key,
    size_t key_len,
    h2_pal_json_value_t **out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = NULL;
    if (object == NULL || key_len == SIZE_MAX ||
        (key == NULL && key_len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->object_get == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result = api->vtable->object_get(
        api->user, object, key, key_len, out_value);
    if (result != H2_PAL_OK) {
        *out_value = NULL;
    }
    return result;
}

/**
 * @brief Return the number of members in an object.
 *
 * The result is independent of object member ordering. `out_size` is cleared
 * before validation and on failure.
 */
static inline h2_pal_result_t h2_pal_json_object_size(
    const h2_pal_json_api_t *api,
    const h2_pal_json_value_t *object,
    size_t *out_size) {
    if (out_size == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_size = 0u;
    if (object == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->object_size == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result =
        api->vtable->object_size(api->user, object, out_size);
    if (result != H2_PAL_OK) {
        *out_size = 0u;
    }
    return result;
}

/**
 * @brief Read one object member by provider-local index.
 *
 * The returned key view and value handle are borrowed from the owning
 * document. Member order is not stable across providers or serialization, and
 * callers must not retain an index across mutation of the object subtree.
 */
static inline h2_pal_result_t h2_pal_json_object_entry(
    const h2_pal_json_api_t *api,
    const h2_pal_json_value_t *object,
    size_t index,
    h2_pal_json_string_view_t *out_key,
    h2_pal_json_value_t **out_value) {
    if (out_key == NULL || out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_key->data = NULL;
    out_key->len = 0u;
    *out_value = NULL;
    if (object == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->object_entry == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result = api->vtable->object_entry(
        api->user, object, index, out_key, out_value);
    if (result != H2_PAL_OK) {
        out_key->data = NULL;
        out_key->len = 0u;
        *out_value = NULL;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_object_set(
    const h2_pal_json_api_t *api,
    h2_pal_json_value_t *object,
    const char *key,
    size_t key_len,
    h2_pal_json_value_t *value) {
    if (object == NULL || value == NULL || key_len == SIZE_MAX ||
        (key == NULL && key_len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->object_set == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->object_set(api->user, object, key, key_len, value);
}

static inline h2_pal_result_t h2_pal_json_document_serialize(
    const h2_pal_json_api_t *api,
    const h2_pal_json_document_t *document,
    h2_pal_json_buffer_t *out_buffer) {
    if (out_buffer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_buffer->data = NULL;
    out_buffer->len = 0u;
    if (document == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->document_serialize == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_pal_result_t result =
        api->vtable->document_serialize(api->user, document, out_buffer);
    if (result != H2_PAL_OK) {
        out_buffer->data = NULL;
        out_buffer->len = 0u;
    }
    return result;
}

static inline h2_pal_result_t h2_pal_json_buffer_release(
    const h2_pal_json_api_t *api,
    h2_pal_json_buffer_t *buffer) {
    if (buffer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (buffer->data == NULL && buffer->len == 0u) {
        return H2_PAL_OK;
    }
    if (buffer->data == NULL) {
        buffer->len = 0u;
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (api->vtable == NULL || api->vtable->buffer_release == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->buffer_release(api->user, buffer);
}

#ifdef __cplusplus
}
#endif

#endif
