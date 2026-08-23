#include "h2_bundle_manifest.h"

#include "h2_bundle_path.h"

#include <stdint.h>
#include <string.h>

static int is_hex(char ch) {
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static int parse_uint64(const char *text, size_t len, uint64_t *out) {
    uint64_t value = 0u;

    if (text == NULL || out == NULL || len == 0u) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < len; ++i) {
        char ch = text[i];
        if (ch < '0' || ch > '9') {
            return H2_BUNDLE_ERR_MANIFEST;
        }
        value = value * 10u + (uint64_t)(ch - '0');
    }
    *out = value;
    return H2_BUNDLE_OK;
}

int h2_bundle_manifest_is_hex_digest(const char *text, size_t len) {
    if (text == NULL || (len != 8u && len != 32u && len != 64u)) {
        return 0;
    }
    for (size_t i = 0u; i < len; ++i) {
        if (!is_hex(text[i])) {
            return 0;
        }
    }
    return 1;
}

int h2_bundle_manifest_parse_line(const char *line, h2_bundle_manifest_entry_t *out_entry) {
    const char *size_start;
    const char *path_start;
    size_t checksum_len;
    size_t size_len;
    size_t path_len;
    size_t line_len;
    int rc;

    if (line == NULL || out_entry == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    line_len = strcspn(line, "\r\n");
    checksum_len = strcspn(line, " \t");
    if (!h2_bundle_manifest_is_hex_digest(line, checksum_len)) {
        return H2_BUNDLE_ERR_MANIFEST;
    }

    size_start = line + checksum_len;
    while ((size_t)(size_start - line) < line_len && (*size_start == ' ' || *size_start == '\t')) {
        ++size_start;
    }
    size_len = strcspn(size_start, " \t\r\n");
    path_start = size_start + size_len;
    while ((size_t)(path_start - line) < line_len && (*path_start == ' ' || *path_start == '\t')) {
        ++path_start;
    }
    path_len = line_len - (size_t)(path_start - line);
    if (size_len == 0u || path_len == 0u || path_len >= sizeof(out_entry->path)) {
        return H2_BUNDLE_ERR_MANIFEST;
    }

    memset(out_entry, 0, sizeof(*out_entry));
    memcpy(out_entry->checksum_hex, line, checksum_len);
    out_entry->checksum_hex[checksum_len] = '\0';
    rc = parse_uint64(size_start, size_len, &out_entry->size);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    memcpy(out_entry->path, path_start, path_len);
    out_entry->path[path_len] = '\0';
    return h2_bundle_path_is_safe_relative(out_entry->path) ? H2_BUNDLE_OK : H2_BUNDLE_ERR_UNSAFE_PATH;
}

const h2_bundle_manifest_entry_t *h2_bundle_manifest_find(const h2_bundle_manifest_t *manifest, const char *path) {
    if (manifest == NULL || path == NULL || manifest->entries == NULL) {
        return NULL;
    }
    for (size_t i = 0u; i < manifest->entry_count; ++i) {
        if (strcmp(manifest->entries[i].path, path) == 0) {
            return manifest->entries + i;
        }
    }
    return NULL;
}

static int hex_nibble(char ch, uint8_t *out) {
    if (out == NULL) {
        return 0;
    }
    if (ch >= '0' && ch <= '9') {
        *out = (uint8_t)(ch - '0');
        return 1;
    }
    if (ch >= 'a' && ch <= 'f') {
        *out = (uint8_t)(10 + ch - 'a');
        return 1;
    }
    if (ch >= 'A' && ch <= 'F') {
        *out = (uint8_t)(10 + ch - 'A');
        return 1;
    }
    return 0;
}

static int parse_crc32_hex(const char *text, uint32_t *out) {
    uint32_t value = 0u;

    if (text == NULL || out == NULL || strlen(text) != 8u) {
        return H2_BUNDLE_ERR_MANIFEST;
    }
    for (size_t i = 0u; i < 8u; ++i) {
        uint8_t nibble = 0u;
        if (!hex_nibble(text[i], &nibble)) {
            return H2_BUNDLE_ERR_MANIFEST;
        }
        value = (value << 4u) | nibble;
    }
    *out = value;
    return H2_BUNDLE_OK;
}

int h2_bundle_manifest_verify_crc32(const h2_bundle_manifest_entry_t *entry, uint64_t size, uint32_t crc32_value) {
    uint32_t expected_crc = 0u;
    int rc;

    if (entry == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    if (entry->size != size) {
        return H2_BUNDLE_ERR_MANIFEST;
    }
    rc = parse_crc32_hex(entry->checksum_hex, &expected_crc);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    return expected_crc == crc32_value ? H2_BUNDLE_OK : H2_BUNDLE_ERR_MANIFEST;
}
