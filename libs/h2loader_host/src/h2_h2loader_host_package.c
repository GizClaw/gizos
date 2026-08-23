#include "h2_h2loader_host_package.h"

#include "h2_bundle_tar.h"
#include "h2_h2loader_host_internal.h"

#include <limits.h>
#include <string.h>
#include <zlib.h>

#define H2_H2LOADER_HOST_PACKAGE_IO_SIZE 8192u
#define H2_H2LOADER_HOST_PACKAGE_MANIFEST_MAX 512u

typedef enum package_tar_phase {
    PACKAGE_TAR_HEADER = 1,
    PACKAGE_TAR_PAYLOAD = 2,
    PACKAGE_TAR_PADDING = 3,
} package_tar_phase_t;

typedef struct package_tar_state {
    h2_h2loader_host_catalog_entry_t asset;
    h2_h2loader_host_sha256_t data_sha;
    h2_h2loader_host_sha256_t image_sha;
    h2_bundle_entry_t entry;
    package_tar_phase_t phase;
    uint8_t header[512];
    size_t header_len;
    uint64_t remaining;
    uint64_t padding_remaining;
    size_t entry_index;
    size_t zero_blocks;
    uint8_t manifest[H2_H2LOADER_HOST_PACKAGE_MANIFEST_MAX];
    size_t manifest_len;
    char data_checksum[H2_H2LOADER_HOST_SHA256_HEX_LEN + 2u];
    size_t data_checksum_len;
    uint64_t image_size;
    char last_data_path[H2_BUNDLE_PATH_MAX];
    int manifest_seen;
    int checksum_seen;
    int data_verified;
    int image_seen;
    int image_verified;
    int done;
} package_tar_state_t;

static voidpf package_alloc(voidpf opaque, uInt items, uInt size) {
    const h2_pal_mem_api_t *allocator =
        (const h2_pal_mem_api_t *)opaque;
    size_t item_count = (size_t)items;
    size_t item_size = (size_t)size;

    if (allocator == NULL ||
        (item_size != 0u && item_count > SIZE_MAX / item_size)) {
        return Z_NULL;
    }
    return (voidpf)h2_pal_mem_alloc(
        allocator, item_count * item_size);
}

static void package_free(voidpf opaque, voidpf address) {
    h2_pal_mem_free(
        (const h2_pal_mem_api_t *)opaque, (void *)address);
}

static int path_starts_with(const char *path, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    return strncmp(path, prefix, prefix_len) == 0 && path[prefix_len] != '\0';
}

static h2_pal_result_t parse_manifest_line(
    const char **cursor,
    const char *end,
    const char *key,
    const char **out_value,
    size_t *out_len) {
    const char *line_end;
    size_t key_len;

    if (cursor == NULL || *cursor == NULL || key == NULL ||
        out_value == NULL || out_len == NULL || *cursor > end) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    line_end = memchr(*cursor, '\n', (size_t)(end - *cursor));
    key_len = strlen(key);
    if (line_end == NULL || (size_t)(line_end - *cursor) <= key_len ||
        memcmp(*cursor, key, key_len) != 0 || (*cursor)[key_len] != '=') {
        return H2_PAL_ERR_FORMAT;
    }
    *out_value = *cursor + key_len + 1u;
    *out_len = (size_t)(line_end - *out_value);
    if (*out_len > 0u && (*out_value)[*out_len - 1u] == '\r') {
        *out_len -= 1u;
    }
    if (*out_len == 0u) {
        return H2_PAL_ERR_FORMAT;
    }
    *cursor = line_end + 1u;
    return H2_PAL_OK;
}

static h2_pal_result_t copy_manifest_text(
    char *out,
    size_t out_size,
    const char *value,
    size_t len,
    int identity_token) {
    if (out == NULL || out_size == 0u || value == NULL ||
        len == 0u || len >= out_size) {
        return H2_PAL_ERR_FORMAT;
    }
    for (size_t i = 0u; i < len; ++i) {
        unsigned char byte = (unsigned char)value[i];
        if (identity_token) {
            if (!((byte >= 'a' && byte <= 'z') ||
                  (byte >= '0' && byte <= '9') ||
                  byte == '.' || byte == '_' || byte == '-')) {
                return H2_PAL_ERR_FORMAT;
            }
        } else if (byte < 0x20u || byte > 0x7eu || byte == ' ') {
            return H2_PAL_ERR_FORMAT;
        }
    }
    memcpy(out, value, len);
    out[len] = '\0';
    return H2_PAL_OK;
}

static h2_pal_result_t parse_u64(
    const char *value,
    size_t len,
    uint64_t *out_value) {
    uint64_t parsed = 0u;

    if (value == NULL || len == 0u || out_value == NULL) {
        return H2_PAL_ERR_FORMAT;
    }
    for (size_t i = 0u; i < len; ++i) {
        uint64_t digit;
        if (value[i] < '0' || value[i] > '9') {
            return H2_PAL_ERR_FORMAT;
        }
        digit = (uint64_t)(value[i] - '0');
        if (parsed > (UINT64_MAX - digit) / 10u) {
            return H2_PAL_ERR_FORMAT;
        }
        parsed = parsed * 10u + digit;
    }
    if (parsed == 0u) {
        return H2_PAL_ERR_FORMAT;
    }
    *out_value = parsed;
    return H2_PAL_OK;
}

static h2_pal_result_t parse_manifest(package_tar_state_t *state) {
    const char *cursor = (const char *)state->manifest;
    const char *end = cursor + state->manifest_len;
    const char *value;
    size_t value_len;
    h2_pal_result_t rc;

#define READ_MANIFEST_LINE(name_literal) \
    do { \
        rc = parse_manifest_line( \
            &cursor, end, name_literal, &value, &value_len); \
        if (rc != H2_PAL_OK) { \
            return rc; \
        } \
    } while (0)

    READ_MANIFEST_LINE("format");
    if (value_len != 1u || value[0] != '1') {
        return H2_PAL_ERR_FORMAT;
    }
    READ_MANIFEST_LINE("role");
    if (value_len == 3u && memcmp(value, "app", 3u) == 0) {
        state->asset.role = H2_H2LOADER_HOST_ASSET_ROLE_APP;
    } else if (value_len == 8u &&
               memcmp(value, "h2loader", 8u) == 0) {
        state->asset.role = H2_H2LOADER_HOST_ASSET_ROLE_LOADER;
    } else {
        return H2_PAL_ERR_FORMAT;
    }
    READ_MANIFEST_LINE("board");
    rc = copy_manifest_text(
        state->asset.board,
        sizeof(state->asset.board),
        value,
        value_len,
        1);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    READ_MANIFEST_LINE("target");
    rc = copy_manifest_text(
        state->asset.target,
        sizeof(state->asset.target),
        value,
        value_len,
        1);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    READ_MANIFEST_LINE("version");
    rc = copy_manifest_text(
        state->asset.version,
        sizeof(state->asset.version),
        value,
        value_len,
        0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (!h2_h2loader_host_is_safe_identity(state->asset.version)) {
        return H2_PAL_ERR_FORMAT;
    }
    READ_MANIFEST_LINE("image_size");
    rc = parse_u64(value, value_len, &state->image_size);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    READ_MANIFEST_LINE("image_sha256");
    if (value_len != H2_H2LOADER_HOST_SHA256_HEX_LEN) {
        return H2_PAL_ERR_FORMAT;
    }
    memcpy(state->asset.image_sha256, value, value_len);
    state->asset.image_sha256[value_len] = '\0';
    if (!h2_h2loader_host_is_sha256(state->asset.image_sha256) ||
        cursor != end) {
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_OK;

#undef READ_MANIFEST_LINE
}

static h2_pal_result_t verify_data_checksum(package_tar_state_t *state) {
    uint8_t digest[32];
    char actual[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];

    if (state->data_verified || !state->checksum_seen) {
        return state->data_verified
            ? H2_PAL_OK
            : H2_PAL_ERR_FORMAT;
    }
    h2_h2loader_host_sha256_finish(&state->data_sha, digest);
    h2_h2loader_host_sha256_hex(digest, actual);
    if (strcmp(actual, state->data_checksum) != 0) {
        return H2_PAL_ERR_FORMAT;
    }
    state->data_verified = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t start_entry(
    package_tar_state_t *state,
    const h2_bundle_entry_t *entry) {
    static const uint8_t separator = 0u;
    h2_pal_result_t rc;

    if (entry->kind != H2_BUNDLE_ENTRY_FILE || state->image_seen) {
        return H2_PAL_ERR_FORMAT;
    }
    state->entry = *entry;
    state->remaining = entry->size;
    state->padding_remaining = h2_bundle_tar_padding(entry->size);
    state->phase = PACKAGE_TAR_PAYLOAD;

    if (strcmp(entry->path, "manifest") == 0) {
        if (state->entry_index != 0u || entry->size == 0u ||
            entry->size > sizeof(state->manifest)) {
            return H2_PAL_ERR_FORMAT;
        }
        state->manifest_seen = 1;
    } else if (strcmp(entry->path, "checksum") == 0) {
        if (state->entry_index != 1u || !state->manifest_seen ||
            entry->size < H2_H2LOADER_HOST_SHA256_HEX_LEN ||
            entry->size > H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u) {
            return H2_PAL_ERR_FORMAT;
        }
        state->checksum_seen = 1;
    } else if (path_starts_with(entry->path, "data/")) {
        if (!state->checksum_seen ||
            state->asset.role != H2_H2LOADER_HOST_ASSET_ROLE_APP ||
            (state->last_data_path[0] != '\0' &&
             strcmp(state->last_data_path, entry->path) >= 0) ||
            strlen(entry->path) >= sizeof(state->last_data_path)) {
            return H2_PAL_ERR_FORMAT;
        }
        memcpy(
            state->last_data_path,
            entry->path,
            strlen(entry->path) + 1u);
        h2_h2loader_host_sha256_update(
            &state->data_sha,
            (const uint8_t *)entry->path,
            strlen(entry->path));
        h2_h2loader_host_sha256_update(
            &state->data_sha, &separator, sizeof(separator));
    } else if (path_starts_with(entry->path, "app/")) {
        rc = verify_data_checksum(state);
        if (rc != H2_PAL_OK || entry->size == 0u ||
            entry->size != state->image_size) {
            return H2_PAL_ERR_FORMAT;
        }
        state->image_seen = 1;
        h2_h2loader_host_sha256_init(&state->image_sha);
    } else {
        return H2_PAL_ERR_FORMAT;
    }
    ++state->entry_index;
    return H2_PAL_OK;
}

static h2_pal_result_t finish_entry(package_tar_state_t *state) {
    static const uint8_t separator = 0u;

    if (strcmp(state->entry.path, "manifest") == 0) {
        h2_pal_result_t rc = parse_manifest(state);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        h2_h2loader_host_sha256_init(&state->data_sha);
    } else if (strcmp(state->entry.path, "checksum") == 0) {
        if (state->data_checksum_len ==
                H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u &&
            state->data_checksum[H2_H2LOADER_HOST_SHA256_HEX_LEN] == '\n') {
            --state->data_checksum_len;
        }
        state->data_checksum[state->data_checksum_len] = '\0';
        if (state->data_checksum_len !=
                H2_H2LOADER_HOST_SHA256_HEX_LEN ||
            !h2_h2loader_host_is_sha256(state->data_checksum)) {
            return H2_PAL_ERR_FORMAT;
        }
    } else if (path_starts_with(state->entry.path, "data/")) {
        h2_h2loader_host_sha256_update(
            &state->data_sha, &separator, sizeof(separator));
    } else if (path_starts_with(state->entry.path, "app/")) {
        uint8_t digest[32];
        char actual[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
        h2_h2loader_host_sha256_finish(&state->image_sha, digest);
        h2_h2loader_host_sha256_hex(digest, actual);
        if (strcmp(actual, state->asset.image_sha256) != 0) {
            return H2_PAL_ERR_FORMAT;
        }
        state->image_verified = 1;
    }
    state->phase = state->padding_remaining > 0u
        ? PACKAGE_TAR_PADDING
        : PACKAGE_TAR_HEADER;
    return H2_PAL_OK;
}

static h2_pal_result_t consume_header(
    package_tar_state_t *state,
    const uint8_t **cursor,
    size_t *remaining) {
    size_t take = sizeof(state->header) - state->header_len;
    h2_bundle_entry_t entry;
    int result;
    int rc;

    if (take > *remaining) {
        take = *remaining;
    }
    memcpy(state->header + state->header_len, *cursor, take);
    state->header_len += take;
    *cursor += take;
    *remaining -= take;
    if (state->header_len != sizeof(state->header)) {
        return H2_PAL_OK;
    }
    state->header_len = 0u;
    rc = h2_bundle_tar_parse_header(state->header, &entry, &result);
    if (rc != H2_BUNDLE_OK) {
        return H2_PAL_ERR_FORMAT;
    }
    if (result == H2_BUNDLE_TAR_HEADER_ZERO) {
        ++state->zero_blocks;
        if (state->zero_blocks >= 2u) {
            state->done = 1;
        }
        return H2_PAL_OK;
    }
    if (state->zero_blocks != 0u || state->done) {
        return H2_PAL_ERR_FORMAT;
    }
    return start_entry(state, &entry);
}

static h2_pal_result_t consume_payload(
    package_tar_state_t *state,
    const uint8_t **cursor,
    size_t *remaining) {
    size_t take = state->remaining > *remaining
        ? *remaining
        : (size_t)state->remaining;

    if (take > 0u) {
        if (strcmp(state->entry.path, "manifest") == 0) {
            if (take > sizeof(state->manifest) - state->manifest_len) {
                return H2_PAL_ERR_FORMAT;
            }
            memcpy(
                state->manifest + state->manifest_len, *cursor, take);
            state->manifest_len += take;
        } else if (strcmp(state->entry.path, "checksum") == 0) {
            if (take > sizeof(state->data_checksum) -
                    state->data_checksum_len - 1u) {
                return H2_PAL_ERR_FORMAT;
            }
            memcpy(
                state->data_checksum + state->data_checksum_len,
                *cursor,
                take);
            state->data_checksum_len += take;
        } else if (path_starts_with(state->entry.path, "data/")) {
            h2_h2loader_host_sha256_update(&state->data_sha, *cursor, take);
        } else if (path_starts_with(state->entry.path, "app/")) {
            h2_h2loader_host_sha256_update(&state->image_sha, *cursor, take);
        }
        state->remaining -= take;
        *cursor += take;
        *remaining -= take;
    }
    return state->remaining == 0u
        ? finish_entry(state)
        : H2_PAL_OK;
}

static h2_pal_result_t consume_padding(
    package_tar_state_t *state,
    const uint8_t **cursor,
    size_t *remaining) {
    size_t take = state->padding_remaining > *remaining
        ? *remaining
        : (size_t)state->padding_remaining;

    for (size_t i = 0u; i < take; ++i) {
        if ((*cursor)[i] != 0u) {
            return H2_PAL_ERR_FORMAT;
        }
    }
    state->padding_remaining -= take;
    *cursor += take;
    *remaining -= take;
    if (state->padding_remaining == 0u) {
        state->phase = PACKAGE_TAR_HEADER;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t feed_tar(
    package_tar_state_t *state,
    const uint8_t *data,
    size_t len) {
    const uint8_t *cursor = data;
    size_t remaining = len;

    while (remaining > 0u) {
        h2_pal_result_t rc;
        if (state->phase == PACKAGE_TAR_HEADER) {
            rc = consume_header(state, &cursor, &remaining);
        } else if (state->phase == PACKAGE_TAR_PAYLOAD) {
            rc = consume_payload(state, &cursor, &remaining);
        } else {
            rc = consume_padding(state, &cursor, &remaining);
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t finish_tar(const package_tar_state_t *state) {
    return state->done && state->header_len == 0u &&
            state->phase == PACKAGE_TAR_HEADER &&
            state->manifest_seen && state->checksum_seen &&
            state->data_verified && state->image_seen &&
            state->image_verified
        ? H2_PAL_OK
        : H2_PAL_ERR_TRUNCATED;
}

static h2_pal_result_t map_zlib_error(int rc) {
    return rc == Z_MEM_ERROR
        ? H2_PAL_ERR_NO_MEMORY
        : H2_PAL_ERR_FORMAT;
}

h2_pal_result_t h2_h2loader_host_package_inspect(
    const h2_h2loader_host_package_inspect_config_t *config,
    h2_h2loader_host_catalog_entry_t *out_asset) {
    h2_h2loader_host_sha256_t archive_sha;
    package_tar_state_t tar;
    z_stream stream;
    uint8_t input[H2_H2LOADER_HOST_PACKAGE_IO_SIZE];
    uint8_t output[H2_H2LOADER_HOST_PACKAGE_IO_SIZE];
    uint8_t digest[32];
    uint64_t offset = 0u;
    int stream_ended = 0;
    h2_pal_result_t result = H2_PAL_OK;
    int rc;

    if (out_asset != NULL) {
        memset(out_asset, 0, sizeof(*out_asset));
    }
    if (config == NULL || out_asset == NULL ||
        config->allocator == NULL || config->read_payload == NULL ||
        config->payload_bytes == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(&tar, 0, sizeof(tar));
    tar.phase = PACKAGE_TAR_HEADER;
    memset(&stream, 0, sizeof(stream));
    stream.zalloc = package_alloc;
    stream.zfree = package_free;
    stream.opaque = (voidpf)config->allocator;
    rc = inflateInit(&stream);
    if (rc != Z_OK) {
        return map_zlib_error(rc);
    }
    h2_h2loader_host_sha256_init(&archive_sha);
    while (offset < config->payload_bytes && result == H2_PAL_OK) {
        size_t request = config->payload_bytes - offset > sizeof(input)
            ? sizeof(input)
            : (size_t)(config->payload_bytes - offset);
        size_t read = 0u;

        result = config->read_payload(
            config->payload_user,
            offset,
            input,
            request,
            &read);
        if (result != H2_PAL_OK) {
            break;
        }
        if (read == 0u || read > request) {
            result = H2_PAL_ERR_TRUNCATED;
            break;
        }
        h2_h2loader_host_sha256_update(&archive_sha, input, read);
        offset += read;
        stream.next_in = input;
        stream.avail_in = (uInt)read;
        while (stream.avail_in > 0u && result == H2_PAL_OK) {
            uInt before = stream.avail_in;
            size_t produced;

            stream.next_out = output;
            stream.avail_out = (uInt)sizeof(output);
            rc = inflate(&stream, Z_NO_FLUSH);
            produced = sizeof(output) - stream.avail_out;
            if (produced > 0u) {
                result = feed_tar(&tar, output, produced);
            }
            if (rc == Z_STREAM_END) {
                stream_ended = 1;
                if (stream.avail_in != 0u ||
                    offset != config->payload_bytes) {
                    result = H2_PAL_ERR_FORMAT;
                }
                break;
            }
            if (rc != Z_OK) {
                result = map_zlib_error(rc);
                break;
            }
            if (produced == 0u && stream.avail_in == before) {
                result = H2_PAL_ERR_FORMAT;
            }
        }
    }
    if (result == H2_PAL_OK && !stream_ended) {
        result = H2_PAL_ERR_TRUNCATED;
    }
    if (result == H2_PAL_OK) {
        size_t trailing = 0u;
        result = config->read_payload(
            config->payload_user,
            offset,
            input,
            1u,
            &trailing);
        if (result == H2_PAL_OK && trailing != 0u) {
            result = H2_PAL_ERR_FORMAT;
        }
    }
    if (result == H2_PAL_OK) {
        result = finish_tar(&tar);
    }
    (void)inflateEnd(&stream);
    if (result != H2_PAL_OK) {
        return result;
    }
    h2_h2loader_host_sha256_finish(&archive_sha, digest);
    h2_h2loader_host_sha256_hex(digest, tar.asset.sha256);
    tar.asset.bytes = config->payload_bytes;
    tar.asset.operation =
        H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL;
    tar.asset.identity_source =
        H2_H2LOADER_HOST_ASSET_IDENTITY_PACKAGE_MANIFEST;
    *out_asset = tar.asset;
    return H2_PAL_OK;
}
