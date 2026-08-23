#include "h2_yyjson_json.h"

#include "h2/pal/h2_pal_unsupported.h"
#include "h2_yyjson_json_internal.h"

#include <math.h>
#include <string.h>

static void *yyjson_alloc(void *context, size_t size) {
    h2_yyjson_json_t *provider = context;
    return h2_pal_mem_alloc(&provider->mem, size);
}

static void *yyjson_realloc(
    void *context,
    void *pointer,
    size_t old_size,
    size_t size) {
    h2_yyjson_json_t *provider = context;
    (void)old_size;
    return h2_pal_mem_realloc(&provider->mem, pointer, size);
}

static void yyjson_free(void *context, void *pointer) {
    h2_yyjson_json_t *provider = context;
    h2_pal_mem_free(&provider->mem, pointer);
}

static h2_pal_result_t resolve_limits(
    const h2_pal_json_limits_t *requested,
    h2_pal_json_limits_t *out_limits) {
    const h2_pal_json_limits_t defaults = {
        .max_document_bytes = H2_PAL_JSON_DEFAULT_MAX_DOCUMENT_BYTES,
        .max_depth = H2_PAL_JSON_DEFAULT_MAX_DEPTH,
        .max_values = H2_PAL_JSON_DEFAULT_MAX_VALUES,
    };
    if (requested == NULL) {
        *out_limits = defaults;
        return H2_PAL_OK;
    }
    if (requested->max_document_bytes == 0u ||
        requested->max_document_bytes > defaults.max_document_bytes ||
        requested->max_depth == 0u ||
        requested->max_depth > defaults.max_depth ||
        requested->max_values == 0u ||
        requested->max_values > defaults.max_values) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_limits = *requested;
    return H2_PAL_OK;
}

static bool utf8_sequence_valid(
    const uint8_t *data,
    size_t len,
    size_t *out_width) {
    const uint8_t first = data[0];
    size_t width = 0u;
    uint32_t codepoint = 0u;
    uint32_t minimum = 0u;
    if (first >= 0xc2u && first <= 0xdfu) {
        width = 2u;
        codepoint = first & 0x1fu;
        minimum = 0x80u;
    } else if (first >= 0xe0u && first <= 0xefu) {
        width = 3u;
        codepoint = first & 0x0fu;
        minimum = 0x800u;
    } else if (first >= 0xf0u && first <= 0xf4u) {
        width = 4u;
        codepoint = first & 0x07u;
        minimum = 0x10000u;
    } else {
        return false;
    }
    if (width > len) return false;
    for (size_t index = 1u; index < width; ++index) {
        if ((data[index] & 0xc0u) != 0x80u) return false;
        codepoint = (codepoint << 6u) | (data[index] & 0x3fu);
    }
    if (codepoint < minimum || codepoint > 0x10ffffu ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
        return false;
    }
    *out_width = width;
    return true;
}

static bool utf8_valid(const char *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)(const void *)data;
    for (size_t index = 0u; index < len;) {
        if (bytes[index] == 0u) return false;
        if (bytes[index] < 0x80u) {
            ++index;
            continue;
        }
        size_t width = 0u;
        if (!utf8_sequence_valid(bytes + index, len - index, &width)) {
            return false;
        }
        index += width;
    }
    return true;
}

static h2_pal_json_document_t *allocate_document(
    h2_yyjson_json_t *provider,
    const h2_pal_json_limits_t *limits) {
    h2_pal_json_document_t *document =
        h2_pal_mem_alloc(&provider->mem, sizeof(*document));
    if (document == NULL) return NULL;
    memset(document, 0, sizeof(*document));
    document->provider = provider;
    document->limits = *limits;
    ++provider->live_documents;
    return document;
}

static h2_pal_json_value_t *allocate_value(
    h2_pal_json_document_t *document,
    bool mutable_value,
    void *node,
    h2_pal_json_value_t *parent,
    bool attached) {
    if (document->value_handles >= document->limits.max_values) return NULL;
    h2_pal_json_value_t *value =
        h2_pal_mem_alloc(&document->provider->mem, sizeof(*value));
    if (value == NULL) return NULL;
    memset(value, 0, sizeof(*value));
    value->document = document;
    value->parent = parent;
    value->mutable_value = mutable_value;
    if (mutable_value) {
        value->node.mutable_value = node;
    } else {
        value->node.parsed = node;
    }
    value->attached = attached;
    value->next = document->values;
    document->values = value;
    ++document->value_handles;
    return value;
}

static void *value_node(const h2_pal_json_value_t *value) {
    return value->mutable_value
        ? (void *)value->node.mutable_value
        : (void *)value->node.parsed;
}

static h2_pal_json_value_t *find_value(
    const h2_pal_json_document_t *document,
    bool mutable_value,
    const void *node) {
    for (h2_pal_json_value_t *value = document->values;
         value != NULL; value = value->next) {
        if (!value->stale && value->mutable_value == mutable_value &&
            value_node(value) == node) {
            return value;
        }
    }
    return NULL;
}

static bool immutable_object_has_duplicates(yyjson_val *object) {
    yyjson_obj_iter outer = yyjson_obj_iter_with(object);
    yyjson_val *outer_key = NULL;
    size_t outer_index = 0u;
    while ((outer_key = yyjson_obj_iter_next(&outer)) != NULL) {
        const char *outer_data = yyjson_get_str(outer_key);
        const size_t outer_len = yyjson_get_len(outer_key);
        yyjson_obj_iter inner = yyjson_obj_iter_with(object);
        yyjson_val *inner_key = NULL;
        size_t inner_index = 0u;
        while ((inner_key = yyjson_obj_iter_next(&inner)) != NULL) {
            if (inner_index > outer_index && yyjson_get_len(inner_key) == outer_len &&
                memcmp(yyjson_get_str(inner_key), outer_data, outer_len) == 0) {
                return true;
            }
            ++inner_index;
        }
        ++outer_index;
    }
    return false;
}

static h2_pal_result_t validate_immutable_tree(
    h2_pal_json_document_t *document,
    yyjson_val *node,
    h2_pal_json_value_t *parent,
    size_t depth,
    h2_pal_json_value_t **out_value) {
    if (depth > document->limits.max_depth ||
        document->value_handles >= document->limits.max_values) {
        return H2_PAL_ERR_NO_SPACE;
    }
    const yyjson_type type = yyjson_get_type(node);
    if (type == YYJSON_TYPE_NONE || type == YYJSON_TYPE_RAW) {
        return H2_PAL_ERR_FORMAT;
    }
    if (type == YYJSON_TYPE_NUM && !isfinite(yyjson_get_num(node))) {
        return H2_PAL_ERR_FORMAT;
    }
    if (type == YYJSON_TYPE_STR &&
        !utf8_valid(yyjson_get_str(node), yyjson_get_len(node))) {
        return H2_PAL_ERR_FORMAT;
    }
    if (type == YYJSON_TYPE_OBJ && immutable_object_has_duplicates(node)) {
        return H2_PAL_ERR_FORMAT;
    }
    h2_pal_json_value_t *value =
        allocate_value(document, false, node, parent, true);
    if (value == NULL) {
        return document->value_handles >= document->limits.max_values
            ? H2_PAL_ERR_NO_SPACE
            : H2_PAL_ERR_NO_MEMORY;
    }
    if (out_value != NULL) *out_value = value;
    if (type == YYJSON_TYPE_ARR) {
        yyjson_arr_iter iterator = yyjson_arr_iter_with(node);
        yyjson_val *child = NULL;
        while ((child = yyjson_arr_iter_next(&iterator)) != NULL) {
            h2_pal_result_t result = validate_immutable_tree(
                document, child, value, depth + 1u, NULL);
            if (result != H2_PAL_OK) return result;
        }
    } else if (type == YYJSON_TYPE_OBJ) {
        yyjson_obj_iter iterator = yyjson_obj_iter_with(node);
        yyjson_val *key = NULL;
        while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
            if (!utf8_valid(yyjson_get_str(key), yyjson_get_len(key))) {
                return H2_PAL_ERR_FORMAT;
            }
            h2_pal_result_t result = validate_immutable_tree(
                document, yyjson_obj_iter_get_val(key), value,
                depth + 1u, NULL);
            if (result != H2_PAL_OK) return result;
        }
    }
    return H2_PAL_OK;
}

static void free_value_handles(h2_pal_json_document_t *document) {
    h2_pal_json_value_t *value = document->values;
    while (value != NULL) {
        h2_pal_json_value_t *next = value->next;
        h2_pal_mem_free(&document->provider->mem, value);
        value = next;
    }
}

static h2_pal_result_t validate_document(
    h2_yyjson_json_t *provider,
    const h2_pal_json_document_t *document) {
    return document->provider == provider
        ? H2_PAL_OK
        : H2_PAL_ERR_INVALID_ARG;
}

static h2_pal_result_t validate_value(
    h2_yyjson_json_t *provider,
    const h2_pal_json_value_t *value) {
    if (value->document->provider != provider) return H2_PAL_ERR_INVALID_ARG;
    return value->stale || value_node(value) == NULL
        ? H2_PAL_ERR_INVALID_STATE
        : H2_PAL_OK;
}

static size_t value_top_depth(const h2_pal_json_value_t *value) {
    size_t depth = 1u;
    while (value->parent != NULL) {
        ++depth;
        value = value->parent;
    }
    return depth;
}

static size_t value_height(const h2_pal_json_value_t *value) {
    size_t height = 1u;
    for (h2_pal_json_value_t *candidate = value->document->values;
         candidate != NULL; candidate = candidate->next) {
        if (!candidate->stale && candidate->parent == value) {
            const size_t child_height = 1u + value_height(candidate);
            if (child_height > height) height = child_height;
        }
    }
    return height;
}

static bool would_cycle(
    const h2_pal_json_value_t *parent,
    const h2_pal_json_value_t *value) {
    for (const h2_pal_json_value_t *cursor = parent;
         cursor != NULL; cursor = cursor->parent) {
        if (cursor == value) return true;
    }
    return false;
}

static h2_pal_result_t validate_attachment(
    h2_pal_json_value_t *parent,
    h2_pal_json_value_t *value) {
    if (parent->document != value->document) return H2_PAL_ERR_INVALID_ARG;
    if (value->attached) return H2_PAL_ERR_INVALID_STATE;
    if (would_cycle(parent, value)) return H2_PAL_ERR_INVALID_ARG;
    const size_t parent_depth = value_top_depth(parent);
    const size_t child_height = value_height(value);
    if (child_height > parent->document->limits.max_depth ||
        parent_depth > parent->document->limits.max_depth - child_height) {
        return H2_PAL_ERR_NO_SPACE;
    }
    return H2_PAL_OK;
}

static h2_pal_json_type_t convert_type(yyjson_type type) {
    switch (type) {
        case YYJSON_TYPE_NULL: return H2_PAL_JSON_TYPE_NULL;
        case YYJSON_TYPE_BOOL: return H2_PAL_JSON_TYPE_BOOLEAN;
        case YYJSON_TYPE_NUM: return H2_PAL_JSON_TYPE_NUMBER;
        case YYJSON_TYPE_STR: return H2_PAL_JSON_TYPE_STRING;
        case YYJSON_TYPE_ARR: return H2_PAL_JSON_TYPE_ARRAY;
        case YYJSON_TYPE_OBJ: return H2_PAL_JSON_TYPE_OBJECT;
        default: return H2_PAL_JSON_TYPE_INVALID;
    }
}

static yyjson_type value_type_raw(const h2_pal_json_value_t *value) {
    return value->mutable_value
        ? yyjson_mut_get_type(value->node.mutable_value)
        : yyjson_get_type(value->node.parsed);
}

static h2_pal_result_t yyjson_document_parse(
    void *user, const uint8_t *data, size_t len,
    const h2_pal_json_limits_t *requested_limits,
    h2_pal_json_document_t **out_document) {
    h2_yyjson_json_t *provider = user;
    h2_pal_json_limits_t limits;
    h2_pal_result_t result = resolve_limits(requested_limits, &limits);
    if (result != H2_PAL_OK) return result;
    if (len > limits.max_document_bytes) return H2_PAL_ERR_NO_SPACE;
    yyjson_read_err error = {0};
    yyjson_doc *parsed = yyjson_read_opts(
        (char *)(void *)(uintptr_t)data, len, YYJSON_READ_NOFLAG,
        &provider->allocator, &error);
    if (parsed == NULL) {
        return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
            ? H2_PAL_ERR_NO_MEMORY
            : H2_PAL_ERR_FORMAT;
    }
    h2_pal_json_document_t *document =
        allocate_document(provider, &limits);
    if (document == NULL) {
        yyjson_doc_free(parsed);
        return H2_PAL_ERR_NO_MEMORY;
    }
    document->parsed_document = parsed;
    result = validate_immutable_tree(
        document, yyjson_doc_get_root(parsed), NULL, 1u, &document->root);
    if (result != H2_PAL_OK) {
        yyjson_doc_free(parsed);
        free_value_handles(document);
        --provider->live_documents;
        h2_pal_mem_free(&provider->mem, document);
        return result;
    }
    *out_document = document;
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_document_create(
    void *user, const h2_pal_json_limits_t *requested_limits,
    h2_pal_json_document_t **out_document) {
    h2_yyjson_json_t *provider = user;
    h2_pal_json_limits_t limits;
    h2_pal_result_t result = resolve_limits(requested_limits, &limits);
    if (result != H2_PAL_OK) return result;
    h2_pal_json_document_t *document = allocate_document(provider, &limits);
    if (document == NULL) return H2_PAL_ERR_NO_MEMORY;
    document->mutable_document = yyjson_mut_doc_new(&provider->allocator);
    if (document->mutable_document == NULL) {
        --provider->live_documents;
        h2_pal_mem_free(&provider->mem, document);
        return H2_PAL_ERR_NO_MEMORY;
    }
    *out_document = document;
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_document_destroy(
    void *user, h2_pal_json_document_t **document_pointer) {
    h2_yyjson_json_t *provider = user;
    h2_pal_json_document_t *document = *document_pointer;
    h2_pal_result_t result = validate_document(provider, document);
    if (result != H2_PAL_OK) return result;
    if (document->parsed_document != NULL) {
        yyjson_doc_free(document->parsed_document);
    } else {
        yyjson_mut_doc_free(document->mutable_document);
    }
    free_value_handles(document);
    --provider->live_documents;
    h2_pal_mem_free(&provider->mem, document);
    *document_pointer = NULL;
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_document_root(
    void *user, h2_pal_json_document_t *document,
    h2_pal_json_value_t **out_value) {
    h2_pal_result_t result = validate_document(user, document);
    if (result != H2_PAL_OK) return result;
    if (document->root == NULL) return H2_PAL_ERR_NOT_FOUND;
    *out_value = document->root;
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_document_set_root(
    void *user, h2_pal_json_document_t *document,
    h2_pal_json_value_t *value) {
    h2_pal_result_t result = validate_document(user, document);
    if (result != H2_PAL_OK) return result;
    result = validate_value(user, value);
    if (result != H2_PAL_OK) return result;
    if (document->mutable_document == NULL || document->root != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (value->document != document) return H2_PAL_ERR_INVALID_ARG;
    if (value->attached) return H2_PAL_ERR_INVALID_STATE;
    if (value_height(value) > document->limits.max_depth) {
        return H2_PAL_ERR_NO_SPACE;
    }
    yyjson_mut_doc_set_root(document->mutable_document, value->node.mutable_value);
    value->attached = true;
    document->root = value;
    return H2_PAL_OK;
}

static h2_pal_result_t validate_mutable_document(
    h2_yyjson_json_t *provider,
    h2_pal_json_document_t *document) {
    h2_pal_result_t result = validate_document(provider, document);
    if (result != H2_PAL_OK) return result;
    return document->mutable_document != NULL
        ? H2_PAL_OK
        : H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t create_mutable_value(
    h2_pal_json_document_t *document,
    yyjson_mut_val *node,
    h2_pal_json_value_t **out_value) {
    if (document->value_handles >= document->limits.max_values) {
        return H2_PAL_ERR_NO_SPACE;
    }
    if (node == NULL) return H2_PAL_ERR_NO_MEMORY;
    h2_pal_json_value_t *value =
        allocate_value(document, true, node, NULL, false);
    if (value == NULL) return H2_PAL_ERR_NO_MEMORY;
    *out_value = value;
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_value_create_null(
    void *user, h2_pal_json_document_t *document,
    h2_pal_json_value_t **out_value) {
    h2_pal_result_t result = validate_mutable_document(user, document);
    if (result != H2_PAL_OK) return result;
    return create_mutable_value(
        document, yyjson_mut_null(document->mutable_document), out_value);
}

static h2_pal_result_t yyjson_value_create_boolean(
    void *user, h2_pal_json_document_t *document, bool input,
    h2_pal_json_value_t **out_value) {
    h2_pal_result_t result = validate_mutable_document(user, document);
    if (result != H2_PAL_OK) return result;
    return create_mutable_value(
        document, yyjson_mut_bool(document->mutable_document, input), out_value);
}

static h2_pal_result_t yyjson_value_create_number(
    void *user, h2_pal_json_document_t *document, double input,
    h2_pal_json_value_t **out_value) {
    if (!isfinite(input)) return H2_PAL_ERR_INVALID_ARG;
    h2_pal_result_t result = validate_mutable_document(user, document);
    if (result != H2_PAL_OK) return result;
    return create_mutable_value(
        document, yyjson_mut_real(document->mutable_document, input), out_value);
}

static h2_pal_result_t yyjson_value_create_string(
    void *user, h2_pal_json_document_t *document, const char *data,
    size_t len, h2_pal_json_value_t **out_value) {
    if (len == SIZE_MAX || !utf8_valid(data == NULL ? "" : data, len)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t result = validate_mutable_document(user, document);
    if (result != H2_PAL_OK) return result;
    return create_mutable_value(
        document,
        yyjson_mut_strncpy(
            document->mutable_document, data == NULL ? "" : data, len),
        out_value);
}

static h2_pal_result_t yyjson_value_create_array(
    void *user, h2_pal_json_document_t *document,
    h2_pal_json_value_t **out_value) {
    h2_pal_result_t result = validate_mutable_document(user, document);
    if (result != H2_PAL_OK) return result;
    return create_mutable_value(
        document, yyjson_mut_arr(document->mutable_document), out_value);
}

static h2_pal_result_t yyjson_value_create_object(
    void *user, h2_pal_json_document_t *document,
    h2_pal_json_value_t **out_value) {
    h2_pal_result_t result = validate_mutable_document(user, document);
    if (result != H2_PAL_OK) return result;
    return create_mutable_value(
        document, yyjson_mut_obj(document->mutable_document), out_value);
}

static h2_pal_result_t yyjson_value_type(
    void *user, const h2_pal_json_value_t *value,
    h2_pal_json_type_t *out_type) {
    h2_pal_result_t result = validate_value(user, value);
    if (result != H2_PAL_OK) return result;
    *out_type = convert_type(value_type_raw(value));
    return *out_type == H2_PAL_JSON_TYPE_INVALID
        ? H2_PAL_ERR_INVALID_STATE
        : H2_PAL_OK;
}

static h2_pal_result_t yyjson_value_get_boolean(
    void *user, const h2_pal_json_value_t *value, bool *out_value) {
    h2_pal_result_t result = validate_value(user, value);
    if (result != H2_PAL_OK) return result;
    if (value_type_raw(value) != YYJSON_TYPE_BOOL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_value = value->mutable_value
        ? yyjson_mut_get_bool(value->node.mutable_value)
        : yyjson_get_bool(value->node.parsed);
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_value_get_number(
    void *user, const h2_pal_json_value_t *value, double *out_value) {
    h2_pal_result_t result = validate_value(user, value);
    if (result != H2_PAL_OK) return result;
    if (value_type_raw(value) != YYJSON_TYPE_NUM) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_value = value->mutable_value
        ? yyjson_mut_get_num(value->node.mutable_value)
        : yyjson_get_num(value->node.parsed);
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_value_get_string(
    void *user, const h2_pal_json_value_t *value,
    h2_pal_json_string_view_t *out_value) {
    h2_pal_result_t result = validate_value(user, value);
    if (result != H2_PAL_OK) return result;
    if (value_type_raw(value) != YYJSON_TYPE_STR) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    out_value->data = value->mutable_value
        ? yyjson_mut_get_str(value->node.mutable_value)
        : yyjson_get_str(value->node.parsed);
    out_value->len = value->mutable_value
        ? yyjson_mut_get_len(value->node.mutable_value)
        : yyjson_get_len(value->node.parsed);
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_array_size(
    void *user, const h2_pal_json_value_t *array, size_t *out_size) {
    h2_pal_result_t result = validate_value(user, array);
    if (result != H2_PAL_OK) return result;
    if (value_type_raw(array) != YYJSON_TYPE_ARR) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_size = array->mutable_value
        ? yyjson_mut_arr_size(array->node.mutable_value)
        : yyjson_arr_size(array->node.parsed);
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_array_get(
    void *user, const h2_pal_json_value_t *array, size_t index,
    h2_pal_json_value_t **out_value) {
    h2_pal_result_t result = validate_value(user, array);
    if (result != H2_PAL_OK) return result;
    if (value_type_raw(array) != YYJSON_TYPE_ARR) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    void *node = array->mutable_value
        ? (void *)yyjson_mut_arr_get(array->node.mutable_value, index)
        : (void *)yyjson_arr_get(array->node.parsed, index);
    if (node == NULL) return H2_PAL_ERR_NOT_FOUND;
    *out_value = find_value(array->document, array->mutable_value, node);
    return *out_value == NULL ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
}

static h2_pal_result_t yyjson_array_append(
    void *user, h2_pal_json_value_t *array, h2_pal_json_value_t *value) {
    h2_pal_result_t result = validate_value(user, array);
    if (result != H2_PAL_OK) return result;
    result = validate_value(user, value);
    if (result != H2_PAL_OK) return result;
    if (!array->mutable_value || value_type_raw(array) != YYJSON_TYPE_ARR) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    result = validate_attachment(array, value);
    if (result != H2_PAL_OK) return result;
    if (!yyjson_mut_arr_append(
            array->node.mutable_value, value->node.mutable_value)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    value->parent = array;
    value->attached = true;
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_object_get(
    void *user, const h2_pal_json_value_t *object, const char *key,
    size_t key_len, h2_pal_json_value_t **out_value) {
    h2_pal_result_t result = validate_value(user, object);
    if (result != H2_PAL_OK) return result;
    if (value_type_raw(object) != YYJSON_TYPE_OBJ) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (key_len == SIZE_MAX ||
        !utf8_valid(key == NULL ? "" : key, key_len)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    void *node = object->mutable_value
        ? (void *)yyjson_mut_obj_getn(
            object->node.mutable_value, key == NULL ? "" : key, key_len)
        : (void *)yyjson_obj_getn(
            object->node.parsed, key == NULL ? "" : key, key_len);
    if (node == NULL) return H2_PAL_ERR_NOT_FOUND;
    *out_value = find_value(object->document, object->mutable_value, node);
    return *out_value == NULL ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
}

static h2_pal_result_t yyjson_object_size(
    void *user, const h2_pal_json_value_t *object, size_t *out_size) {
    h2_pal_result_t result = validate_value(user, object);
    if (result != H2_PAL_OK) return result;
    if (value_type_raw(object) != YYJSON_TYPE_OBJ) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_size = object->mutable_value
        ? yyjson_mut_obj_size(object->node.mutable_value)
        : yyjson_obj_size(object->node.parsed);
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_object_entry(
    void *user, const h2_pal_json_value_t *object, size_t index,
    h2_pal_json_string_view_t *out_key,
    h2_pal_json_value_t **out_value) {
    h2_pal_result_t result = validate_value(user, object);
    if (result != H2_PAL_OK) return result;
    if (value_type_raw(object) != YYJSON_TYPE_OBJ) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    void *key_node = NULL;
    void *value_node_pointer = NULL;
    if (object->mutable_value) {
        yyjson_mut_obj_iter iterator =
            yyjson_mut_obj_iter_with(object->node.mutable_value);
        yyjson_mut_val *key = NULL;
        for (size_t current = 0u;
             (key = yyjson_mut_obj_iter_next(&iterator)) != NULL;
             ++current) {
            if (current == index) {
                key_node = key;
                value_node_pointer = yyjson_mut_obj_iter_get_val(key);
                break;
            }
        }
        if (key_node != NULL) {
            out_key->data = yyjson_mut_get_str((yyjson_mut_val *)key_node);
            out_key->len = yyjson_mut_get_len((yyjson_mut_val *)key_node);
        }
    } else {
        yyjson_obj_iter iterator = yyjson_obj_iter_with(object->node.parsed);
        yyjson_val *key = NULL;
        for (size_t current = 0u;
             (key = yyjson_obj_iter_next(&iterator)) != NULL;
             ++current) {
            if (current == index) {
                key_node = key;
                value_node_pointer = yyjson_obj_iter_get_val(key);
                break;
            }
        }
        if (key_node != NULL) {
            out_key->data = yyjson_get_str((yyjson_val *)key_node);
            out_key->len = yyjson_get_len((yyjson_val *)key_node);
        }
    }
    if (key_node == NULL || value_node_pointer == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_value = find_value(
        object->document, object->mutable_value, value_node_pointer);
    return *out_value == NULL ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
}

static void mark_stale_subtree(h2_pal_json_value_t *root) {
    for (h2_pal_json_value_t *candidate = root->document->values;
         candidate != NULL; candidate = candidate->next) {
        h2_pal_json_value_t *cursor = candidate;
        while (cursor != NULL && cursor != root) cursor = cursor->parent;
        if (cursor == root) candidate->stale = true;
    }
}

static h2_pal_result_t yyjson_object_set(
    void *user, h2_pal_json_value_t *object, const char *key,
    size_t key_len, h2_pal_json_value_t *value) {
    h2_pal_result_t result = validate_value(user, object);
    if (result != H2_PAL_OK) return result;
    result = validate_value(user, value);
    if (result != H2_PAL_OK) return result;
    if (!object->mutable_value || value_type_raw(object) != YYJSON_TYPE_OBJ) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (key_len == SIZE_MAX ||
        !utf8_valid(key == NULL ? "" : key, key_len)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    result = validate_attachment(object, value);
    if (result != H2_PAL_OK) return result;
    yyjson_mut_val *old_node = yyjson_mut_obj_getn(
        object->node.mutable_value, key == NULL ? "" : key, key_len);
    h2_pal_json_value_t *old_value = old_node == NULL
        ? NULL
        : find_value(object->document, true, old_node);
    if (old_node != NULL && old_value == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    yyjson_mut_val *key_value = yyjson_mut_strncpy(
        object->document->mutable_document, key == NULL ? "" : key, key_len);
    if (key_value == NULL) return H2_PAL_ERR_NO_MEMORY;
    if (!yyjson_mut_obj_put(
            object->node.mutable_value, key_value, value->node.mutable_value)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (old_value != NULL) mark_stale_subtree(old_value);
    value->parent = object;
    value->attached = true;
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_document_serialize(
    void *user, const h2_pal_json_document_t *document,
    h2_pal_json_buffer_t *out_buffer) {
    h2_yyjson_json_t *provider = user;
    h2_pal_result_t result = validate_document(provider, document);
    if (result != H2_PAL_OK) return result;
    if (document->root == NULL) return H2_PAL_ERR_NOT_FOUND;
    yyjson_write_err error = {0};
    size_t len = 0u;
    char *data = document->mutable_document != NULL
        ? yyjson_mut_write_opts(
            document->mutable_document, YYJSON_WRITE_NOFLAG,
            &provider->allocator, &len, &error)
        : yyjson_write_opts(
            document->parsed_document, YYJSON_WRITE_NOFLAG,
            &provider->allocator, &len, &error);
    if (data == NULL) {
        return error.code == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION
            ? H2_PAL_ERR_NO_MEMORY
            : H2_PAL_ERR_INVALID_STATE;
    }
    if (len > document->limits.max_document_bytes) {
        provider->allocator.free(provider, data);
        return H2_PAL_ERR_NO_SPACE;
    }
    h2_yyjson_buffer_record_t *record =
        h2_pal_mem_alloc(&provider->mem, sizeof(*record));
    if (record == NULL) {
        provider->allocator.free(provider, data);
        return H2_PAL_ERR_NO_MEMORY;
    }
    record->data = (uint8_t *)(void *)data;
    record->next = provider->buffers;
    provider->buffers = record;
    ++provider->live_buffers;
    out_buffer->data = record->data;
    out_buffer->len = len;
    return H2_PAL_OK;
}

static h2_pal_result_t yyjson_buffer_release(
    void *user, h2_pal_json_buffer_t *buffer) {
    h2_yyjson_json_t *provider = user;
    h2_yyjson_buffer_record_t **record = &provider->buffers;
    while (*record != NULL && (*record)->data != buffer->data) {
        record = &(*record)->next;
    }
    if (*record == NULL) return H2_PAL_ERR_INVALID_ARG;
    h2_yyjson_buffer_record_t *released = *record;
    *record = released->next;
    provider->allocator.free(provider, released->data);
    h2_pal_mem_free(&provider->mem, released);
    --provider->live_buffers;
    buffer->data = NULL;
    buffer->len = 0u;
    return H2_PAL_OK;
}

static const h2_pal_json_vtable_t yyjson_vtable = {
    .document_parse = yyjson_document_parse,
    .document_create = yyjson_document_create,
    .document_destroy = yyjson_document_destroy,
    .document_root = yyjson_document_root,
    .document_set_root = yyjson_document_set_root,
    .value_create_null = yyjson_value_create_null,
    .value_create_boolean = yyjson_value_create_boolean,
    .value_create_number = yyjson_value_create_number,
    .value_create_string = yyjson_value_create_string,
    .value_create_array = yyjson_value_create_array,
    .value_create_object = yyjson_value_create_object,
    .value_type = yyjson_value_type,
    .value_get_boolean = yyjson_value_get_boolean,
    .value_get_number = yyjson_value_get_number,
    .value_get_string = yyjson_value_get_string,
    .array_size = yyjson_array_size,
    .array_get = yyjson_array_get,
    .array_append = yyjson_array_append,
    .object_get = yyjson_object_get,
    .object_size = yyjson_object_size,
    .object_entry = yyjson_object_entry,
    .object_set = yyjson_object_set,
    .document_serialize = yyjson_document_serialize,
    .buffer_release = yyjson_buffer_release,
};

h2_pal_result_t h2_yyjson_json_create(
    const h2_pal_mem_api_t *mem,
    h2_yyjson_json_t **out_provider) {
    if (out_provider == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_provider = NULL;
    if (mem == NULL || mem->vtable == NULL || mem->vtable->alloc == NULL ||
        mem->vtable->realloc == NULL || mem->vtable->free == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_yyjson_json_t *provider = h2_pal_mem_alloc(mem, sizeof(*provider));
    if (provider == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(provider, 0, sizeof(*provider));
    provider->mem = *mem;
    provider->api.user = provider;
    provider->api.vtable = &yyjson_vtable;
    provider->allocator.malloc = yyjson_alloc;
    provider->allocator.realloc = yyjson_realloc;
    provider->allocator.free = yyjson_free;
    provider->allocator.ctx = provider;
    *out_provider = provider;
    return H2_PAL_OK;
}

const h2_pal_json_api_t *h2_yyjson_json_api(h2_yyjson_json_t *provider) {
    return provider == NULL ? h2_pal_unsupported_json_api() : &provider->api;
}

h2_pal_result_t h2_yyjson_json_destroy(h2_yyjson_json_t **provider_pointer) {
    if (provider_pointer == NULL) return H2_PAL_ERR_INVALID_ARG;
    h2_yyjson_json_t *provider = *provider_pointer;
    if (provider == NULL) return H2_PAL_OK;
    if (provider->live_documents != 0u || provider->live_buffers != 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_mem_api_t mem = provider->mem;
    h2_pal_mem_free(&mem, provider);
    *provider_pointer = NULL;
    return H2_PAL_OK;
}
