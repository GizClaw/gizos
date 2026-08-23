#include "h2_bundle_tar.h"

#include "h2_bundle_path.h"

#include <string.h>

static int is_zero_block(const uint8_t block[512]) {
    for (size_t i = 0u; i < 512u; ++i) {
        if (block[i] != 0u) {
            return 0;
        }
    }
    return 1;
}

static int parse_octal(const uint8_t *field, size_t len, uint64_t *out) {
    uint64_t value = 0u;
    size_t i = 0u;

    if (field == NULL || out == NULL || len == 0u) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    while (i < len && (field[i] == ' ' || field[i] == '\0')) {
        ++i;
    }
    for (; i < len; ++i) {
        uint8_t ch = field[i];
        if (ch == '\0' || ch == ' ') {
            break;
        }
        if (ch < '0' || ch > '7') {
            return H2_BUNDLE_ERR_TAR;
        }
        value = (value << 3u) + (uint64_t)(ch - '0');
    }
    *out = value;
    return H2_BUNDLE_OK;
}

static int verify_checksum(const uint8_t block[512]) {
    uint64_t stored = 0u;
    uint64_t sum = 0u;
    int rc = parse_octal(block + 148u, 8u, &stored);

    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    for (size_t i = 0u; i < 512u; ++i) {
        if (i >= 148u && i < 156u) {
            sum += (uint8_t)' ';
        } else {
            sum += block[i];
        }
    }
    return sum == stored ? H2_BUNDLE_OK : H2_BUNDLE_ERR_TAR;
}

static size_t field_len(const uint8_t *field, size_t max_len) {
    size_t len = 0u;
    while (len < max_len && field[len] != '\0') {
        ++len;
    }
    return len;
}

static int copy_path(char *out, size_t out_len, const uint8_t *name, size_t name_max, const uint8_t *prefix, size_t prefix_max) {
    size_t name_len = field_len(name, name_max);
    size_t prefix_len = field_len(prefix, prefix_max);
    size_t offset = 0u;

    if (name_len == 0u) {
        return H2_BUNDLE_ERR_TAR;
    }
    if (prefix_len > 0u) {
        if (prefix_len + 1u + name_len + 1u > out_len) {
            return H2_BUNDLE_ERR_NO_SPACE;
        }
        memcpy(out, prefix, prefix_len);
        offset = prefix_len;
        out[offset++] = '/';
    } else if (name_len + 1u > out_len) {
        return H2_BUNDLE_ERR_NO_SPACE;
    }
    memcpy(out + offset, name, name_len);
    out[offset + name_len] = '\0';
    return H2_BUNDLE_OK;
}

int h2_bundle_tar_parse_header(const uint8_t block[512], h2_bundle_entry_t *out_entry, int *out_header_result) {
    uint64_t size = 0u;
    char typeflag;
    int rc;

    if (block == NULL || out_entry == NULL || out_header_result == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    if (is_zero_block(block)) {
        *out_header_result = H2_BUNDLE_TAR_HEADER_ZERO;
        memset(out_entry, 0, sizeof(*out_entry));
        return H2_BUNDLE_OK;
    }

    rc = verify_checksum(block);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    rc = parse_octal(block + 124u, 12u, &size);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    rc = copy_path(out_entry->path, sizeof(out_entry->path), block, 100u, block + 345u, 155u);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }

    typeflag = (char)block[156u];
    if (typeflag == '\0' || typeflag == '0') {
        if (!h2_bundle_path_is_safe_relative(out_entry->path)) {
            return H2_BUNDLE_ERR_UNSAFE_PATH;
        }
        out_entry->kind = H2_BUNDLE_ENTRY_FILE;
        out_entry->size = size;
    } else if (typeflag == '5') {
        size_t path_len = strlen(out_entry->path);
        if (size != 0u) {
            return H2_BUNDLE_ERR_TAR;
        }
        while (path_len > 0u && out_entry->path[path_len - 1u] == '/') {
            out_entry->path[--path_len] = '\0';
        }
        if (!h2_bundle_path_is_safe_relative(out_entry->path)) {
            return H2_BUNDLE_ERR_UNSAFE_PATH;
        }
        out_entry->kind = H2_BUNDLE_ENTRY_DIR;
        out_entry->size = 0u;
    } else {
        return H2_BUNDLE_ERR_UNSUPPORTED_ENTRY;
    }

    *out_header_result = H2_BUNDLE_TAR_HEADER_ENTRY;
    return H2_BUNDLE_OK;
}

uint64_t h2_bundle_tar_padding(uint64_t size) {
    uint64_t mod = size & 511u;
    return mod == 0u ? 0u : 512u - mod;
}
