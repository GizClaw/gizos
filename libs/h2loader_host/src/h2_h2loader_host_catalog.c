#include "h2_h2loader_host_catalog.h"

#include "h2_h2loader_host_internal.h"

#include <limits.h>
#include <string.h>

typedef enum json_token_kind {
    JSON_TOKEN_UNDEFINED = 0,
    JSON_TOKEN_OBJECT,
    JSON_TOKEN_ARRAY,
    JSON_TOKEN_STRING,
    JSON_TOKEN_PRIMITIVE,
} json_token_kind_t;

typedef struct json_token {
    json_token_kind_t kind;
    int start;
    int end;
    int size;
    int parent;
} json_token_t;

typedef struct json_parser {
    size_t position;
    size_t next_token;
    int parent;
} json_parser_t;

struct h2_h2loader_host_catalog {
    const h2_pal_mem_api_t *allocator;
    h2_h2loader_host_catalog_entry_t *entries;
    size_t count;
};

static json_token_t *json_allocate_token(
    json_parser_t *parser,
    json_token_t *tokens,
    size_t token_capacity) {
    json_token_t *token;

    if (parser->next_token >= token_capacity) {
        return NULL;
    }
    token = &tokens[parser->next_token++];
    token->kind = JSON_TOKEN_UNDEFINED;
    token->start = -1;
    token->end = -1;
    token->size = 0;
    token->parent = -1;
    return token;
}

static int json_parse_string(
    json_parser_t *parser,
    const uint8_t *json,
    size_t json_len,
    json_token_t *tokens,
    size_t token_capacity) {
    size_t start = parser->position + 1u;

    for (++parser->position; parser->position < json_len;
         ++parser->position) {
        uint8_t byte = json[parser->position];
        if (byte == '"') {
            json_token_t *token =
                json_allocate_token(parser, tokens, token_capacity);
            if (token == NULL) {
                return -1;
            }
            token->kind = JSON_TOKEN_STRING;
            token->start = (int)start;
            token->end = (int)parser->position;
            token->parent = parser->parent;
            return 0;
        }
        if (byte == '\\') {
            ++parser->position;
            if (parser->position >= json_len) {
                return -2;
            }
            byte = json[parser->position];
            if (byte == 'u') {
                for (size_t i = 0u; i < 4u; ++i) {
                    if (++parser->position >= json_len) {
                        return -2;
                    }
                    byte = json[parser->position];
                    if (!((byte >= '0' && byte <= '9') ||
                          (byte >= 'a' && byte <= 'f') ||
                          (byte >= 'A' && byte <= 'F'))) {
                        return -2;
                    }
                }
            } else if (strchr("\"/bfnrt\\", byte) == NULL) {
                return -2;
            }
        } else if (byte < 0x20u) {
            return -2;
        }
    }
    return -2;
}

static int json_parse_primitive(
    json_parser_t *parser,
    const uint8_t *json,
    size_t json_len,
    json_token_t *tokens,
    size_t token_capacity) {
    size_t start = parser->position;

    while (parser->position < json_len) {
        uint8_t byte = json[parser->position];
        if (byte == ',' || byte == ']' || byte == '}' ||
            byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
            break;
        }
        if (byte < 0x20u || byte >= 0x7fu || byte == ':' || byte == '[' ||
            byte == '{' || byte == '"') {
            return -2;
        }
        ++parser->position;
    }
    if (parser->position == start) {
        return -2;
    }
    json_token_t *token =
        json_allocate_token(parser, tokens, token_capacity);
    if (token == NULL) {
        return -1;
    }
    token->kind = JSON_TOKEN_PRIMITIVE;
    token->start = (int)start;
    token->end = (int)parser->position;
    token->parent = parser->parent;
    if (parser->position > 0u) {
        --parser->position;
    }
    return 0;
}

#define JSON_MAX_NESTING 64u

static void json_validate_skip_space(
    const uint8_t *json,
    size_t json_len,
    size_t *position) {
    while (*position < json_len) {
        uint8_t byte = json[*position];
        if (byte != ' ' && byte != '\t' &&
            byte != '\r' && byte != '\n') {
            break;
        }
        ++*position;
    }
}

static int json_validate_string(
    const uint8_t *json,
    size_t json_len,
    size_t *position) {
    if (*position >= json_len || json[*position] != '"') {
        return 0;
    }
    for (++*position; *position < json_len; ++*position) {
        uint8_t byte = json[*position];
        if (byte == '"') {
            ++*position;
            return 1;
        }
        if (byte == '\\') {
            if (++*position >= json_len) {
                return 0;
            }
            byte = json[*position];
            if (byte == 'u') {
                for (size_t i = 0u; i < 4u; ++i) {
                    if (++*position >= json_len) {
                        return 0;
                    }
                    byte = json[*position];
                    if (!((byte >= '0' && byte <= '9') ||
                          (byte >= 'a' && byte <= 'f') ||
                          (byte >= 'A' && byte <= 'F'))) {
                        return 0;
                    }
                }
            } else if (strchr("\"/bfnrt\\", byte) == NULL) {
                return 0;
            }
        } else if (byte < 0x20u) {
            return 0;
        }
    }
    return 0;
}

static int json_validate_number(
    const uint8_t *json,
    size_t json_len,
    size_t *position) {
    size_t cursor = *position;
    if (cursor < json_len && json[cursor] == '-') {
        ++cursor;
    }
    if (cursor >= json_len) {
        return 0;
    }
    if (json[cursor] == '0') {
        ++cursor;
    } else {
        if (json[cursor] < '1' || json[cursor] > '9') {
            return 0;
        }
        do {
            ++cursor;
        } while (cursor < json_len &&
                 json[cursor] >= '0' && json[cursor] <= '9');
    }
    if (cursor < json_len && json[cursor] == '.') {
        ++cursor;
        if (cursor >= json_len ||
            json[cursor] < '0' || json[cursor] > '9') {
            return 0;
        }
        do {
            ++cursor;
        } while (cursor < json_len &&
                 json[cursor] >= '0' && json[cursor] <= '9');
    }
    if (cursor < json_len &&
        (json[cursor] == 'e' || json[cursor] == 'E')) {
        ++cursor;
        if (cursor < json_len &&
            (json[cursor] == '+' || json[cursor] == '-')) {
            ++cursor;
        }
        if (cursor >= json_len ||
            json[cursor] < '0' || json[cursor] > '9') {
            return 0;
        }
        do {
            ++cursor;
        } while (cursor < json_len &&
                 json[cursor] >= '0' && json[cursor] <= '9');
    }
    *position = cursor;
    return 1;
}

static int json_validate_value(
    const uint8_t *json,
    size_t json_len,
    size_t *position,
    size_t depth);

static int json_validate_object(
    const uint8_t *json,
    size_t json_len,
    size_t *position,
    size_t depth) {
    ++*position;
    json_validate_skip_space(json, json_len, position);
    if (*position < json_len && json[*position] == '}') {
        ++*position;
        return 1;
    }
    for (;;) {
        if (!json_validate_string(json, json_len, position)) {
            return 0;
        }
        json_validate_skip_space(json, json_len, position);
        if (*position >= json_len || json[*position] != ':') {
            return 0;
        }
        ++*position;
        if (!json_validate_value(
                json, json_len, position, depth + 1u)) {
            return 0;
        }
        json_validate_skip_space(json, json_len, position);
        if (*position >= json_len) {
            return 0;
        }
        if (json[*position] == '}') {
            ++*position;
            return 1;
        }
        if (json[*position] != ',') {
            return 0;
        }
        ++*position;
        json_validate_skip_space(json, json_len, position);
    }
}

static int json_validate_array(
    const uint8_t *json,
    size_t json_len,
    size_t *position,
    size_t depth) {
    ++*position;
    json_validate_skip_space(json, json_len, position);
    if (*position < json_len && json[*position] == ']') {
        ++*position;
        return 1;
    }
    for (;;) {
        if (!json_validate_value(
                json, json_len, position, depth + 1u)) {
            return 0;
        }
        json_validate_skip_space(json, json_len, position);
        if (*position >= json_len) {
            return 0;
        }
        if (json[*position] == ']') {
            ++*position;
            return 1;
        }
        if (json[*position] != ',') {
            return 0;
        }
        ++*position;
        json_validate_skip_space(json, json_len, position);
    }
}

static int json_validate_value(
    const uint8_t *json,
    size_t json_len,
    size_t *position,
    size_t depth) {
    if (depth > JSON_MAX_NESTING) {
        return 0;
    }
    json_validate_skip_space(json, json_len, position);
    if (*position >= json_len) {
        return 0;
    }
    if (json[*position] == '{') {
        return json_validate_object(
            json, json_len, position, depth);
    }
    if (json[*position] == '[') {
        return json_validate_array(
            json, json_len, position, depth);
    }
    if (json[*position] == '"') {
        return json_validate_string(json, json_len, position);
    }
    static const char *const literals[] = {
        "true",
        "false",
        "null",
    };
    for (size_t i = 0u;
         i < sizeof(literals) / sizeof(literals[0]);
         ++i) {
        size_t literal_len = strlen(literals[i]);
        if (literal_len <= json_len - *position &&
            memcmp(&json[*position], literals[i], literal_len) == 0) {
            *position += literal_len;
            return 1;
        }
    }
    return json_validate_number(json, json_len, position);
}

static int json_validate_document(
    const uint8_t *json,
    size_t json_len) {
    size_t position = 0u;
    if (!json_validate_value(json, json_len, &position, 0u)) {
        return 0;
    }
    json_validate_skip_space(json, json_len, &position);
    return position == json_len;
}

static int json_tokenize(
    const uint8_t *json,
    size_t json_len,
    json_token_t *tokens,
    size_t token_capacity,
    size_t *out_count) {
    json_parser_t parser = { .parent = -1 };

    if (!json_validate_document(json, json_len)) {
        return -2;
    }
    for (; parser.position < json_len; ++parser.position) {
        uint8_t byte = json[parser.position];
        if (byte == '{' || byte == '[') {
            json_token_t *token =
                json_allocate_token(&parser, tokens, token_capacity);
            if (token == NULL) {
                return -1;
            }
            if (parser.parent >= 0) {
                ++tokens[parser.parent].size;
            }
            token->kind =
                byte == '{' ? JSON_TOKEN_OBJECT : JSON_TOKEN_ARRAY;
            token->start = (int)parser.position;
            token->parent = parser.parent;
            parser.parent = (int)parser.next_token - 1;
        } else if (byte == '}' || byte == ']') {
            json_token_kind_t expected =
                byte == '}' ? JSON_TOKEN_OBJECT : JSON_TOKEN_ARRAY;
            int index = (int)parser.next_token - 1;
            for (; index >= 0; --index) {
                if (tokens[index].start >= 0 && tokens[index].end < 0) {
                    if (tokens[index].kind != expected) {
                        return -2;
                    }
                    tokens[index].end = (int)parser.position + 1;
                    parser.parent = tokens[index].parent;
                    break;
                }
            }
            if (index < 0) {
                return -2;
            }
        } else if (byte == '"') {
            int rc = json_parse_string(
                &parser, json, json_len, tokens, token_capacity);
            if (rc != 0) {
                return rc;
            }
            if (parser.parent >= 0) {
                ++tokens[parser.parent].size;
            }
        } else if (byte == ' ' || byte == '\t' || byte == '\r' ||
                   byte == '\n' || byte == ':' || byte == ',') {
            continue;
        } else {
            int rc = json_parse_primitive(
                &parser, json, json_len, tokens, token_capacity);
            if (rc != 0) {
                return rc;
            }
            if (parser.parent >= 0) {
                ++tokens[parser.parent].size;
            }
        }
    }
    for (size_t i = 0u; i < parser.next_token; ++i) {
        if (tokens[i].start >= 0 && tokens[i].end < 0) {
            return -2;
        }
    }
    *out_count = parser.next_token;
    return 0;
}

static int json_token_equals(
    const uint8_t *json,
    const json_token_t *token,
    const char *text) {
    size_t text_len = strlen(text);
    return token != NULL && token->kind == JSON_TOKEN_STRING &&
        token->start >= 0 && token->end >= token->start &&
        (size_t)(token->end - token->start) == text_len &&
        memcmp(&json[token->start], text, text_len) == 0;
}

static size_t json_token_next(
    const json_token_t *tokens,
    size_t token_count,
    size_t index) {
    int end;

    if (index >= token_count) {
        return token_count;
    }
    end = tokens[index].end;
    ++index;
    while (index < token_count && tokens[index].start < end) {
        ++index;
    }
    return index;
}

static const json_token_t *json_object_value(
    const uint8_t *json,
    const json_token_t *tokens,
    size_t token_count,
    size_t object_index,
    const char *key,
    size_t *out_index) {
    size_t index = object_index + 1u;

    if (object_index >= token_count ||
        tokens[object_index].kind != JSON_TOKEN_OBJECT) {
        return NULL;
    }
    while (index < token_count &&
           tokens[index].start < tokens[object_index].end) {
        size_t value_index = index + 1u;
        if (tokens[index].parent != (int)object_index ||
            tokens[index].kind != JSON_TOKEN_STRING ||
            value_index >= token_count ||
            tokens[value_index].parent != (int)object_index) {
            return NULL;
        }
        if (json_token_equals(json, &tokens[index], key)) {
            if (out_index != NULL) {
                *out_index = value_index;
            }
            return &tokens[value_index];
        }
        index = json_token_next(tokens, token_count, value_index);
    }
    return NULL;
}

static int json_string_copy(
    const uint8_t *json,
    const json_token_t *token,
    char *out,
    size_t out_size) {
    if (token == NULL || token->kind != JSON_TOKEN_STRING ||
        token->start < 0 || token->end < token->start) {
        return 0;
    }
    for (int i = token->start; i < token->end; ++i) {
        if (json[i] == '\\') {
            return 0;
        }
    }
    return h2_h2loader_host_copy_text(
        out,
        out_size,
        (const char *)&json[token->start],
        (size_t)(token->end - token->start));
}

static int json_u64(
    const uint8_t *json,
    const json_token_t *token,
    uint64_t *out_value) {
    uint64_t value = 0u;

    if (token == NULL || token->kind != JSON_TOKEN_PRIMITIVE ||
        token->start < 0 || token->end <= token->start) {
        return 0;
    }
    for (int i = token->start; i < token->end; ++i) {
        uint8_t byte = json[i];
        if (byte < '0' || byte > '9' ||
            value > (UINT64_MAX - (uint64_t)(byte - '0')) / 10u) {
            return 0;
        }
        value = value * 10u + (uint64_t)(byte - '0');
    }
    *out_value = value;
    return 1;
}

static h2_pal_result_t verify_resource(
    const h2_h2loader_host_catalog_config_t *config,
    const h2_h2loader_host_catalog_entry_t *entry) {
    h2_h2loader_host_sha256_t sha;
    uint8_t buffer[8192];
    uint8_t digest[32];
    char actual[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
    uint64_t offset = 0u;

    h2_h2loader_host_sha256_init(&sha);
    while (offset < entry->bytes) {
        size_t request = entry->bytes - offset > sizeof(buffer)
            ? sizeof(buffer)
            : (size_t)(entry->bytes - offset);
        size_t read = 0u;
        h2_pal_result_t rc = config->read_resource(
            config->resource_user,
            entry->resource_name,
            offset,
            buffer,
            request,
            &read);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (read == 0u || read > request) {
            return H2_PAL_ERR_TRUNCATED;
        }
        h2_h2loader_host_sha256_update(&sha, buffer, read);
        offset += read;
    }
    size_t trailing = 0u;
    h2_pal_result_t rc = config->read_resource(
        config->resource_user,
        entry->resource_name,
        offset,
        buffer,
        1u,
        &trailing);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (trailing != 0u) {
        return H2_PAL_ERR_FORMAT;
    }
    h2_h2loader_host_sha256_finish(&sha, digest);
    h2_h2loader_host_sha256_hex(digest, actual);
    return strcmp(actual, entry->sha256) == 0
        ? H2_PAL_OK
        : H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t parse_role(
    const uint8_t *json,
    const json_token_t *token,
    h2_h2loader_host_asset_role_t *out_role) {
    if (json_token_equals(json, token, "app")) {
        *out_role = H2_H2LOADER_HOST_ASSET_ROLE_APP;
        return H2_PAL_OK;
    }
    if (json_token_equals(json, token, "h2loader")) {
        *out_role = H2_H2LOADER_HOST_ASSET_ROLE_LOADER;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t parse_operation(
    const uint8_t *json,
    const json_token_t *token,
    h2_h2loader_host_asset_operation_t *out_operation) {
    if (token == NULL ||
        json_token_equals(json, token, "managed-install")) {
        *out_operation = H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL;
        return H2_PAL_OK;
    }
    if (json_token_equals(json, token, "recovery")) {
        *out_operation = H2_H2LOADER_HOST_ASSET_OPERATION_RECOVERY;
        return H2_PAL_OK;
    }
    if (json_token_equals(json, token, "diagnostic")) {
        *out_operation = H2_H2LOADER_HOST_ASSET_OPERATION_DIAGNOSTIC;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t parse_asset(
    const h2_h2loader_host_catalog_config_t *config,
    const uint8_t *json,
    const json_token_t *tokens,
    size_t token_count,
    size_t asset_index,
    const h2_h2loader_host_catalog_entry_t *identity,
    h2_h2loader_host_catalog_entry_t *out_entry) {
    const json_token_t *token;
    uint64_t size = 0u;

    *out_entry = *identity;
    token = json_object_value(
        json, tokens, token_count, asset_index, "name", NULL);
    if (!json_string_copy(
            json,
            token,
            out_entry->resource_name,
            sizeof(out_entry->resource_name)) ||
        !h2_h2loader_host_is_safe_resource_name(out_entry->resource_name)) {
        return H2_PAL_ERR_FORMAT;
    }
    token = json_object_value(
        json, tokens, token_count, asset_index, "sha256", NULL);
    if (!json_string_copy(
            json, token, out_entry->sha256, sizeof(out_entry->sha256)) ||
        !h2_h2loader_host_is_sha256(out_entry->sha256)) {
        return H2_PAL_ERR_FORMAT;
    }
    token = json_object_value(
        json, tokens, token_count, asset_index, "size", NULL);
    if (!json_u64(json, token, &size) || size == 0u) {
        return H2_PAL_ERR_FORMAT;
    }
    out_entry->bytes = size;
    token = json_object_value(
        json, tokens, token_count, asset_index, "operation", NULL);
    h2_pal_result_t rc =
        parse_operation(json, token, &out_entry->operation);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return verify_resource(config, out_entry);
}

static int entry_is_duplicate(
    const h2_h2loader_host_catalog_entry_t *entries,
    size_t count,
    const h2_h2loader_host_catalog_entry_t *candidate) {
    for (size_t i = 0u; i < count; ++i) {
        if (strcmp(entries[i].resource_name, candidate->resource_name) == 0 ||
            (strcmp(entries[i].board, candidate->board) == 0 &&
             strcmp(entries[i].target, candidate->target) == 0 &&
             strcmp(entries[i].image, candidate->image) == 0 &&
             entries[i].role == candidate->role &&
             entries[i].operation == candidate->operation)) {
            return 1;
        }
    }
    return 0;
}

static h2_pal_result_t count_assets(
    const uint8_t *json,
    const json_token_t *tokens,
    size_t token_count,
    size_t firmware_array_index,
    size_t *out_count) {
    size_t count = 0u;
    size_t item_index = firmware_array_index + 1u;

    while (item_index < token_count &&
           tokens[item_index].start < tokens[firmware_array_index].end) {
        size_t assets_index = 0u;
        const json_token_t *assets = json_object_value(
            json,
            tokens,
            token_count,
            item_index,
            "assets",
            &assets_index);
        if (tokens[item_index].parent != (int)firmware_array_index ||
            tokens[item_index].kind != JSON_TOKEN_OBJECT ||
            assets == NULL || assets->kind != JSON_TOKEN_ARRAY ||
            assets->size <= 0) {
            return H2_PAL_ERR_FORMAT;
        }
        if ((size_t)assets->size > SIZE_MAX - count) {
            return H2_PAL_ERR_NO_SPACE;
        }
        count += (size_t)assets->size;
        item_index = json_token_next(tokens, token_count, item_index);
    }
    if (count == 0u) {
        return H2_PAL_ERR_FORMAT;
    }
    *out_count = count;
    return H2_PAL_OK;
}

static h2_pal_result_t parse_identity(
    const uint8_t *json,
    const json_token_t *tokens,
    size_t token_count,
    size_t item_index,
    h2_h2loader_host_catalog_entry_t *out_identity,
    size_t *out_assets_index) {
    const json_token_t *token;
    size_t manifest_index = 0u;

    memset(out_identity, 0, sizeof(*out_identity));
#define H2_PARSE_IDENTITY_FIELD(key_literal, member) \
    token = json_object_value( \
        json, tokens, token_count, item_index, key_literal, NULL); \
    if (!json_string_copy( \
            json, \
            token, \
            out_identity->member, \
            sizeof(out_identity->member)) || \
        !h2_h2loader_host_is_safe_identity(out_identity->member)) { \
        return H2_PAL_ERR_FORMAT; \
    }
    H2_PARSE_IDENTITY_FIELD("board", board)
    H2_PARSE_IDENTITY_FIELD("target", target)
    H2_PARSE_IDENTITY_FIELD("image", image)
    H2_PARSE_IDENTITY_FIELD("version", version)
#undef H2_PARSE_IDENTITY_FIELD
    token = json_object_value(
        json, tokens, token_count, item_index, "role", NULL);
    h2_pal_result_t rc = parse_role(json, token, &out_identity->role);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    token = json_object_value(
        json,
        tokens,
        token_count,
        item_index,
        "package_manifest",
        &manifest_index);
    if (token == NULL || token->kind != JSON_TOKEN_OBJECT) {
        return H2_PAL_ERR_FORMAT;
    }
    token = json_object_value(
        json,
        tokens,
        token_count,
        manifest_index,
        "image_sha256",
        NULL);
    if (!json_string_copy(
            json,
            token,
            out_identity->image_sha256,
            sizeof(out_identity->image_sha256)) ||
        !h2_h2loader_host_is_sha256(out_identity->image_sha256)) {
        return H2_PAL_ERR_FORMAT;
    }
    token = json_object_value(
        json,
        tokens,
        token_count,
        item_index,
        "assets",
        out_assets_index);
    return token != NULL && token->kind == JSON_TOKEN_ARRAY
        ? H2_PAL_OK
        : H2_PAL_ERR_FORMAT;
}

h2_pal_result_t h2_h2loader_host_catalog_open(
    const h2_h2loader_host_catalog_config_t *config,
    h2_h2loader_host_catalog_t **out_catalog) {
    json_token_t *tokens = NULL;
    size_t token_capacity;
    size_t token_count = 0u;
    size_t firmware_index = 0u;
    const json_token_t *token;
    h2_h2loader_host_catalog_t *catalog = NULL;
    h2_pal_result_t rc = H2_PAL_ERR_FORMAT;

    if (out_catalog != NULL) {
        *out_catalog = NULL;
    }
    if (config == NULL || out_catalog == NULL ||
        config->allocator == NULL || config->index_json == NULL ||
        config->index_json_len == 0u || config->read_resource == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (config->index_json_len > 16u * 1024u * 1024u ||
        config->index_json_len > (SIZE_MAX - 128u) / 2u) {
        return H2_PAL_ERR_NO_SPACE;
    }
    token_capacity = config->index_json_len / 2u + 128u;
    if (token_capacity > SIZE_MAX / sizeof(*tokens)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    tokens = h2_pal_mem_alloc(
        config->allocator, token_capacity * sizeof(*tokens));
    if (tokens == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    int token_rc = json_tokenize(
        config->index_json,
        config->index_json_len,
        tokens,
        token_capacity,
        &token_count);
    if (token_rc != 0 || token_count == 0u ||
        tokens[0].kind != JSON_TOKEN_OBJECT) {
        goto cleanup;
    }
    token = json_object_value(
        config->index_json, tokens, token_count, 0u, "format", NULL);
    uint64_t format = 0u;
    if (!json_u64(config->index_json, token, &format) || format != 1u) {
        goto cleanup;
    }
    token = json_object_value(
        config->index_json,
        tokens,
        token_count,
        0u,
        "firmware",
        &firmware_index);
    if (token == NULL || token->kind != JSON_TOKEN_ARRAY) {
        goto cleanup;
    }
    size_t asset_count = 0u;
    rc = count_assets(
        config->index_json,
        tokens,
        token_count,
        firmware_index,
        &asset_count);
    if (rc != H2_PAL_OK) {
        goto cleanup;
    }
    if (asset_count > SIZE_MAX / sizeof(*catalog->entries)) {
        rc = H2_PAL_ERR_NO_SPACE;
        goto cleanup;
    }
    catalog = h2_pal_mem_alloc(config->allocator, sizeof(*catalog));
    if (catalog == NULL) {
        rc = H2_PAL_ERR_NO_MEMORY;
        goto cleanup;
    }
    memset(catalog, 0, sizeof(*catalog));
    catalog->allocator = config->allocator;
    catalog->entries = h2_pal_mem_alloc(
        config->allocator, asset_count * sizeof(*catalog->entries));
    if (catalog->entries == NULL) {
        rc = H2_PAL_ERR_NO_MEMORY;
        goto cleanup;
    }
    size_t item_index = firmware_index + 1u;
    while (item_index < token_count &&
           tokens[item_index].start < tokens[firmware_index].end) {
        h2_h2loader_host_catalog_entry_t identity;
        size_t assets_index = 0u;
        rc = parse_identity(
            config->index_json,
            tokens,
            token_count,
            item_index,
            &identity,
            &assets_index);
        if (rc != H2_PAL_OK) {
            goto cleanup;
        }
        size_t asset_index = assets_index + 1u;
        while (asset_index < token_count &&
               tokens[asset_index].start < tokens[assets_index].end) {
            h2_h2loader_host_catalog_entry_t entry;
            if (tokens[asset_index].parent != (int)assets_index ||
                tokens[asset_index].kind != JSON_TOKEN_OBJECT) {
                rc = H2_PAL_ERR_FORMAT;
                goto cleanup;
            }
            rc = parse_asset(
                config,
                config->index_json,
                tokens,
                token_count,
                asset_index,
                &identity,
                &entry);
            if (rc != H2_PAL_OK ||
                entry_is_duplicate(
                    catalog->entries, catalog->count, &entry)) {
                if (rc == H2_PAL_OK) {
                    rc = H2_PAL_ERR_FORMAT;
                }
                goto cleanup;
            }
            catalog->entries[catalog->count++] = entry;
            asset_index =
                json_token_next(tokens, token_count, asset_index);
        }
        item_index = json_token_next(tokens, token_count, item_index);
    }
    if (catalog->count != asset_count) {
        rc = H2_PAL_ERR_FORMAT;
        goto cleanup;
    }
    *out_catalog = catalog;
    catalog = NULL;
    rc = H2_PAL_OK;

cleanup:
    h2_pal_mem_free(config->allocator, tokens);
    if (catalog != NULL) {
        h2_pal_mem_free(config->allocator, catalog->entries);
        h2_pal_mem_free(config->allocator, catalog);
    }
    return rc;
}

h2_pal_result_t h2_h2loader_host_catalog_close(
    h2_h2loader_host_catalog_t **inout_catalog) {
    h2_h2loader_host_catalog_t *catalog;

    if (inout_catalog == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    catalog = *inout_catalog;
    if (catalog == NULL) {
        return H2_PAL_OK;
    }
    *inout_catalog = NULL;
    h2_pal_mem_free(catalog->allocator, catalog->entries);
    h2_pal_mem_free(catalog->allocator, catalog);
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_catalog_count(
    const h2_h2loader_host_catalog_t *catalog,
    size_t *out_count) {
    if (out_count != NULL) {
        *out_count = 0u;
    }
    if (catalog == NULL || out_count == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_count = catalog->count;
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_catalog_get(
    const h2_h2loader_host_catalog_t *catalog,
    size_t index,
    h2_h2loader_host_catalog_entry_t *out_entry) {
    if (out_entry != NULL) {
        memset(out_entry, 0, sizeof(*out_entry));
    }
    if (catalog == NULL || out_entry == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (index >= catalog->count) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_entry = catalog->entries[index];
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_catalog_find(
    const h2_h2loader_host_catalog_t *catalog,
    const char *board,
    const char *target,
    h2_h2loader_host_asset_role_t role,
    h2_h2loader_host_asset_operation_t operation,
    size_t *out_indices,
    size_t out_capacity,
    size_t *out_count) {
    size_t count = 0u;

    if (out_count != NULL) {
        *out_count = 0u;
    }
    if (catalog == NULL || board == NULL || target == NULL ||
        out_count == NULL || (out_capacity > 0u && out_indices == NULL) ||
        (role != H2_H2LOADER_HOST_ASSET_ROLE_APP &&
         role != H2_H2LOADER_HOST_ASSET_ROLE_LOADER) ||
        (operation != H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL &&
         operation != H2_H2LOADER_HOST_ASSET_OPERATION_RECOVERY &&
         operation != H2_H2LOADER_HOST_ASSET_OPERATION_DIAGNOSTIC)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < catalog->count; ++i) {
        const h2_h2loader_host_catalog_entry_t *entry =
            &catalog->entries[i];
        if (strcmp(entry->board, board) != 0 ||
            strcmp(entry->target, target) != 0 ||
            entry->role != role || entry->operation != operation) {
            continue;
        }
        if (count < out_capacity) {
            out_indices[count] = i;
        }
        ++count;
    }
    *out_count = count;
    return count > out_capacity ? H2_PAL_ERR_NO_SPACE : H2_PAL_OK;
}
