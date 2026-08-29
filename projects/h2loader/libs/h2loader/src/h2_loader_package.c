#include "h2_loader_package.h"

#include "h2_bundle_tar.h"
#include "h2_bundle_types.h"
#include "h2_loader_metadata.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>

#define H2_LOADER_VALIDATE_IO_BUF_SIZE 1024u
#define H2_LOADER_IMAGE_IO_BUF_SIZE (64u * 1024u)
#define H2_LOADER_SHA256_SIZE 32u
#define H2_LOADER_MANIFEST_MAX 512u

typedef struct h2_loader_disk_writer {
    const h2_pal_disk_api_t *disk;
    const char *app_entry_path;
    h2_loader_package_t *package;
    uint32_t partition_id;
    uint64_t offset;
    h2_pal_disk_partition_t partition;
} h2_loader_disk_writer_t;

typedef struct h2_loader_layout_validator {
    const char *app_entry_path;
    h2_loader_digest_api_t *digest;
    h2_loader_package_inspection_t *inspection;
    uint8_t header[512];
    size_t header_len;
    uint64_t remaining;
    uint64_t padding_remaining;
    int reading_header;
    int reading_padding;
    int zero_blocks;
    int checksum_seen;
    uint8_t checksum_data[H2_LOADER_IDENTITY_TEXT_MAX];
    size_t checksum_len;
    int data_digest_started;
    int data_digest_verified;
    char last_data_path[H2_BUNDLE_PATH_MAX];
    int manifest_seen;
    int data_seen;
    int image_digest_started;
    size_t entry_index;
    uint8_t manifest_data[H2_LOADER_MANIFEST_MAX];
    size_t manifest_len;
    int app_seen;
    int done;
    h2_bundle_entry_t entry;
} h2_loader_layout_validator_t;

typedef struct h2_loader_writer_bridge {
    const h2_loader_image_writer_api_t *api;
    const h2_loader_image_identity_t *identity;
    uint32_t partition_id;
    h2_loader_package_t *package;
    uint64_t written;
    int active;
} h2_loader_writer_bridge_t;

static voidpf package_zlib_alloc(voidpf opaque, uInt items, uInt size) {
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

static void package_zlib_free(voidpf opaque, voidpf address) {
    h2_pal_mem_free(
        (const h2_pal_mem_api_t *)opaque, (void *)address);
}

static void install_progress(h2_loader_package_t *package,
                             h2_loader_install_phase_t phase,
                             uint64_t completed, uint64_t total,
                             const char *detail) {
    if (package != NULL && package->config.progress != NULL) {
        package->config.progress(package->config.progress_user, phase,
            completed, total, detail);
    }
}

static void bundle_install_progress(
    void *user,
    const h2_bundle_entry_t *entry,
    const h2_bundle_install_stats_t *stats) {
    h2_loader_package_t *package = (h2_loader_package_t *)user;
    uint64_t delta = 0u;
    if (package == NULL || entry == NULL || stats == NULL) {
        return;
    }
    if (stats->payload_bytes >= package->bundle_progress_bytes) {
        delta = stats->payload_bytes - package->bundle_progress_bytes;
    }
    package->bundle_progress_bytes = stats->payload_bytes;
    if (strcmp(entry->path, "checksum") != 0 &&
        strncmp(entry->path, "app/", 4u) != 0) {
        size_t path_len = strlen(entry->path);
        h2_loader_install_phase_t phase =
            path_len >= 5u && strcmp(entry->path + path_len - 5u, ".pixa") == 0
                ? H2_LOADER_INSTALL_PHASE_PIXA
                : H2_LOADER_INSTALL_PHASE_DATA;
        uint64_t *completed = phase == H2_LOADER_INSTALL_PHASE_PIXA
            ? &package->pixa_progress_bytes : &package->data_progress_bytes;
        uint64_t total = phase == H2_LOADER_INSTALL_PHASE_PIXA
            ? package->pixa_total_bytes : package->data_total_bytes;
        if (UINT64_MAX - *completed < delta) {
            *completed = UINT64_MAX;
        } else {
            *completed += delta;
        }
        install_progress(package, phase, *completed, total, entry->path);
    }
}

static const char *default_if_empty(const char *value, const char *fallback) {
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static int path_starts_with(const char *path, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    return strncmp(path, prefix, prefix_len) == 0;
}

static int path_has_suffix(const char *path, const char *suffix) {
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len &&
        strcmp(path + path_len - suffix_len, suffix) == 0;
}

static int is_safe_relative_path(const char *path) {
    const char *segment = path;

    if (path == NULL || path[0] == '\0' || path[0] == '/' || strchr(path, '\\') != NULL) {
        return 0;
    }
    while (*segment != '\0') {
        const char *slash = strchr(segment, '/');
        size_t len = slash != NULL ? (size_t)(slash - segment) : strlen(segment);

        if (len == 0u ||
            (len == 1u && segment[0] == '.') ||
            (len == 2u && segment[0] == '.' && segment[1] == '.')) {
            return 0;
        }
        if (slash == NULL) {
            break;
        }
        segment = slash + 1;
    }
    return 1;
}

static int is_sha256_hex(const char *value) {
    if (value == NULL || strlen(value) != 64u) {
        return 0;
    }
    for (size_t i = 0u; i < 64u; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
                (value[i] >= 'a' && value[i] <= 'f') ||
                (value[i] >= 'A' && value[i] <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

static int is_lower_sha256_bytes(const uint8_t *value, size_t len) {
    if (value == NULL || len != H2_LOADER_SHA256_HEX_SIZE - 1u) {
        return 0;
    }
    for (size_t i = 0u; i < len; ++i) {
        if (!((value[i] >= (uint8_t)'0' && value[i] <= (uint8_t)'9') ||
              (value[i] >= (uint8_t)'a' && value[i] <= (uint8_t)'f'))) {
            return 0;
        }
    }
    return 1;
}

static int is_lower_sha256_hex(const char *value) {
    if (value == NULL || strlen(value) != 64u) {
        return 0;
    }
    for (size_t i = 0u; i < 64u; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
                (value[i] >= 'a' && value[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int parse_manifest_line(
    const char **cursor,
    const char *end,
    const char *key,
    const char **out_value,
    size_t *out_len) {
    const char *line_end;
    size_t key_len;

    if (cursor == NULL || *cursor == NULL || key == NULL || out_value == NULL || out_len == NULL) {
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

static int copy_manifest_text(char *out, size_t out_size, const char *value, size_t len, int identity_token) {
    if (out == NULL || out_size == 0u || value == NULL || len == 0u || len >= out_size) {
        return H2_PAL_ERR_FORMAT;
    }
    for (size_t i = 0u; i < len; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (identity_token) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                    ch == '.' || ch == '_' || ch == '-')) {
                return H2_PAL_ERR_FORMAT;
            }
        } else if (ch < 0x20u || ch > 0x7eu || ch == ' ') {
            return H2_PAL_ERR_FORMAT;
        }
    }
    memcpy(out, value, len);
    out[len] = '\0';
    return H2_PAL_OK;
}

static int parse_u64_decimal(const char *value, size_t len, uint64_t *out_value) {
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

int h2_loader_package_manifest_parse(
    const void *data,
    size_t len,
    h2_loader_package_manifest_t *out_manifest) {
    const char *cursor;
    const char *end;
    const char *value;
    size_t value_len;
    int rc;

    if (data == NULL || out_manifest == NULL || len == 0u || len > H2_LOADER_MANIFEST_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    cursor = (const char *)data;
    end = cursor + len;
    memset(out_manifest, 0, sizeof(*out_manifest));
#define READ_LINE(name) \
    do { \
        rc = parse_manifest_line(&cursor, end, name, &value, &value_len); \
        if (rc != H2_PAL_OK) { \
            return rc; \
        } \
    } while (0)
    READ_LINE("format");
    if (value_len != 1u || value[0] != '1') {
        return H2_PAL_ERR_FORMAT;
    }
    out_manifest->format = 1u;
    READ_LINE("role");
    if (value_len == 3u && memcmp(value, "app", 3u) == 0) {
        out_manifest->role = H2_LOADER_IMAGE_ROLE_APP;
    } else if (value_len == 8u && memcmp(value, "h2loader", 8u) == 0) {
        out_manifest->role = H2_LOADER_IMAGE_ROLE_H2LOADER;
    } else {
        return H2_PAL_ERR_FORMAT;
    }
    READ_LINE("board");
    rc = copy_manifest_text(out_manifest->board, sizeof(out_manifest->board), value, value_len, 1);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    READ_LINE("target");
    rc = copy_manifest_text(out_manifest->target, sizeof(out_manifest->target), value, value_len, 1);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    READ_LINE("version");
    rc = copy_manifest_text(out_manifest->version, sizeof(out_manifest->version), value, value_len, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    READ_LINE("image_size");
    rc = parse_u64_decimal(value, value_len, &out_manifest->image_size);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    READ_LINE("image_sha256");
    if (value_len != 64u) {
        return H2_PAL_ERR_FORMAT;
    }
    memcpy(out_manifest->image_sha256, value, value_len);
    out_manifest->image_sha256[value_len] = '\0';
    if (!is_lower_sha256_hex(out_manifest->image_sha256) || cursor != end) {
        return H2_PAL_ERR_FORMAT;
    }
#undef READ_LINE
    return H2_PAL_OK;
}

static void digest_to_hex(
    const uint8_t digest[H2_LOADER_SHA256_SIZE],
    char out_hex[H2_LOADER_SHA256_SIZE * 2u + 1u]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < H2_LOADER_SHA256_SIZE; ++i) {
        out_hex[i * 2u] = hex[digest[i] >> 4u];
        out_hex[i * 2u + 1u] = hex[digest[i] & 0x0fu];
    }
    out_hex[H2_LOADER_SHA256_SIZE * 2u] = '\0';
}

int h2_loader_package_verify_path(
    h2_loader_package_t *package,
    const char *archive_path,
    uint64_t expected_size,
    const char *expected_sha256) {
    uint8_t buffer[H2_LOADER_VALIDATE_IO_BUF_SIZE];
    uint8_t digest[H2_LOADER_SHA256_SIZE];
    char actual_checksum[H2_LOADER_SHA256_SIZE * 2u + 1u];
    h2_pal_fs_file_t *archive = NULL;
    h2_pal_fs_stat_t stat;
    int rc;

    if (package == NULL || archive_path == NULL || expected_size == 0u ||
        !is_sha256_hex(expected_sha256) ||
        package->config.digest.start == NULL || package->config.digest.update == NULL ||
        package->config.digest.finish == NULL || package->config.digest.abort == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (package->config.fs != NULL && package->config.fs->vtable != NULL &&
        package->config.fs->vtable->stat != NULL) {
        rc = h2_pal_fs_stat(package->config.fs, archive_path, &stat);
        if (rc != H2_PAL_FS_OK) return rc;
        if (stat.is_dir || stat.size != expected_size) return H2_PAL_ERR_FORMAT;
    }
    rc = h2_pal_fs_open(
        package->config.fs,
        archive_path,
        H2_PAL_FS_OPEN_READ,
        &archive);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    rc = package->config.digest.start(package->config.digest.user);
    if (rc != H2_PAL_OK) {
        (void)h2_pal_fs_close(package->config.fs, archive);
        return rc;
    }
    for (;;) {
        size_t read_len = 0u;
        rc = h2_pal_fs_read(
            package->config.fs,
            archive,
            buffer,
            sizeof(buffer),
            &read_len);
        if (rc != H2_PAL_FS_OK) {
            break;
        }
        if (read_len == 0u) {
            rc = package->config.digest.finish(package->config.digest.user, digest);
            break;
        }
        rc = package->config.digest.update(
            package->config.digest.user,
            buffer,
            read_len);
        if (rc != H2_PAL_OK) {
            break;
        }
    }
    package->config.digest.abort(package->config.digest.user);
    {
        int close_rc = h2_pal_fs_close(package->config.fs, archive);
        if (rc == H2_PAL_OK && close_rc != H2_PAL_FS_OK) {
            rc = close_rc;
        }
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    digest_to_hex(digest, actual_checksum);
    return strcmp(actual_checksum, expected_sha256) == 0 ?
        H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

static int verify_archive_checksum(
    h2_loader_package_t *package,
    const h2_loader_identity_t *identity) {
    if (identity == NULL) return H2_PAL_ERR_INVALID_ARG;
    return h2_loader_package_verify_path(
        package,
        package != NULL ? package->config.package_path : NULL,
        identity->size,
        identity->checksum);
}

static void copy_text(char *dst, size_t dst_len, const char *src) {
    size_t len;

    if (dst == NULL || dst_len == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_len) {
        len = dst_len - 1u;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int select_app_partition(
    const h2_pal_disk_api_t *disk,
    uint32_t partition_id,
    h2_pal_disk_partition_t *out_partition) {
    if (disk == NULL || out_partition == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (partition_id == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_pal_disk_get_partition(disk, partition_id, out_partition);
}

static uint64_t erase_len_for_entry(const h2_pal_disk_partition_t *partition, uint64_t entry_size) {
    uint64_t len = entry_size;
    uint32_t block = partition != NULL ? partition->erase_block_size : 0u;

    if (partition != NULL && partition->size > 0u && partition->size < len) {
        len = partition->size;
    }
    if (block > 0u && len > 0u) {
        uint64_t rem = len % block;
        if (rem != 0u) {
            len += (uint64_t)block - rem;
        }
    }
    if (partition != NULL && partition->size > 0u && len > partition->size) {
        len = partition->size;
    }
    return len;
}

static int disk_writer_begin(void *user, const h2_bundle_entry_t *entry) {
    h2_loader_disk_writer_t *writer = (h2_loader_disk_writer_t *)user;
    uint64_t erase_len;
    int rc;

    if (writer == NULL || entry == NULL || writer->disk == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (strcmp(entry->path, writer->app_entry_path) != 0) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    rc = select_app_partition(writer->disk, writer->partition_id, &writer->partition);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (writer->partition.size > 0u && entry->size > writer->partition.size) {
        return H2_PAL_ERR_NO_SPACE;
    }
    erase_len = erase_len_for_entry(&writer->partition, entry->size);
    if (erase_len > 0u) {
        rc = h2_pal_disk_erase(writer->disk, writer->partition.id, 0u, erase_len);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    writer->offset = 0u;
    install_progress(writer->package, H2_LOADER_INSTALL_PHASE_IMAGE,
        0u, entry->size, entry->path);
    return H2_PAL_OK;
}

static int disk_writer_write(void *user, const h2_bundle_entry_t *entry, const void *data, size_t len) {
    h2_loader_disk_writer_t *writer = (h2_loader_disk_writer_t *)user;
    int rc;

    if (writer == NULL || entry == NULL || writer->disk == NULL ||
        (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return H2_PAL_OK;
    }
    if (writer->partition.size > 0u &&
        (writer->offset > writer->partition.size || (uint64_t)len > writer->partition.size - writer->offset)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    if (writer->offset > entry->size ||
        (uint64_t)len > entry->size - writer->offset) {
        return H2_PAL_ERR_FORMAT;
    }
    rc = h2_pal_disk_write(writer->disk, writer->partition.id, writer->offset, data, len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    writer->offset += len;
    install_progress(writer->package, H2_LOADER_INSTALL_PHASE_IMAGE,
        writer->offset, entry->size, entry->path);
    return H2_PAL_OK;
}

static int disk_writer_end(void *user, const h2_bundle_entry_t *entry) {
    h2_loader_disk_writer_t *writer = (h2_loader_disk_writer_t *)user;

    (void)entry;
    if (writer == NULL || writer->disk == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = h2_pal_disk_flush(writer->disk, writer->partition.id);
    return rc == H2_PAL_ERR_UNSUPPORTED ? H2_PAL_OK : rc;
}

static int writer_bridge_begin(void *user, const h2_bundle_entry_t *entry) {
    h2_loader_writer_bridge_t *bridge = (h2_loader_writer_bridge_t *)user;
    int rc;

    if (bridge == NULL || entry == NULL || bridge->api == NULL || bridge->api->vtable == NULL ||
        bridge->api->vtable->begin == NULL || bridge->identity == NULL ||
        entry->size != bridge->identity->image_size) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = bridge->api->vtable->begin(
        bridge->api->user,
        bridge->partition_id,
        bridge->identity);
    if (rc == H2_PAL_OK) {
        bridge->active = 1;
        bridge->written = 0u;
        install_progress(bridge->package, H2_LOADER_INSTALL_PHASE_IMAGE,
            0u, bridge->identity->image_size, entry->path);
    }
    return rc;
}

static int writer_bridge_write(
    void *user,
    const h2_bundle_entry_t *entry,
    const void *data,
    size_t len) {
    h2_loader_writer_bridge_t *bridge = (h2_loader_writer_bridge_t *)user;
    if (bridge == NULL || entry == NULL || !bridge->active ||
        bridge->api == NULL || bridge->api->vtable == NULL ||
        bridge->api->vtable->write == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (bridge->written > entry->size ||
        (uint64_t)len > entry->size - bridge->written) {
        return H2_PAL_ERR_FORMAT;
    }
    int rc = bridge->api->vtable->write(bridge->api->user, data, len);
    if (rc == H2_PAL_OK) {
        bridge->written += len;
        install_progress(bridge->package, H2_LOADER_INSTALL_PHASE_IMAGE,
            bridge->written, bridge->identity->image_size,
            entry->path);
    }
    return rc;
}

static int writer_bridge_end(void *user, const h2_bundle_entry_t *entry) {
    h2_loader_writer_bridge_t *bridge = (h2_loader_writer_bridge_t *)user;
    int rc;
    (void)entry;
    if (bridge == NULL || !bridge->active || bridge->api == NULL || bridge->api->vtable == NULL ||
        bridge->api->vtable->finish == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    rc = bridge->api->vtable->finish(bridge->api->user, bridge->identity);
    if (rc == H2_PAL_OK) {
        bridge->active = 0;
    }
    return rc;
}

static void writer_bridge_abort(void *user) {
    h2_loader_writer_bridge_t *bridge = (h2_loader_writer_bridge_t *)user;
    if (bridge == NULL || !bridge->active) {
        return;
    }
    if (bridge->api != NULL && bridge->api->vtable != NULL && bridge->api->vtable->abort != NULL) {
        bridge->api->vtable->abort(bridge->api->user);
    }
    bridge->active = 0;
}

static int image_capacity(
    const h2_loader_package_t *package,
    uint32_t partition_id,
    uint64_t *out_capacity) {
    h2_pal_disk_partition_t partition;
    if (package == NULL || partition_id == 0u || out_capacity == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_capacity = 0u;
    if (package->config.image_reader != NULL && package->config.image_reader->vtable != NULL &&
        package->config.image_reader->vtable->get_capacity != NULL) {
        return package->config.image_reader->vtable->get_capacity(
            package->config.image_reader->user,
            partition_id,
            out_capacity);
    }
    if (package->config.image_writer != NULL && package->config.image_writer->vtable != NULL &&
        package->config.image_writer->vtable->get_capacity != NULL) {
        return package->config.image_writer->vtable->get_capacity(
            package->config.image_writer->user,
            partition_id,
            out_capacity);
    }
    if (package->config.disk == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    {
        int rc = h2_pal_disk_get_partition(package->config.disk, partition_id, &partition);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    *out_capacity = partition.size;
    return H2_PAL_OK;
}

static int image_read(
    const h2_loader_package_t *package,
    uint32_t partition_id,
    uint64_t offset,
    void *data,
    size_t len) {
    if (package == NULL || partition_id == 0u || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (package->config.image_reader != NULL && package->config.image_reader->vtable != NULL &&
        package->config.image_reader->vtable->read != NULL) {
        return package->config.image_reader->vtable->read(
            package->config.image_reader->user,
            partition_id,
            offset,
            data,
            len);
    }
    if (package->config.disk == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return h2_pal_disk_read(package->config.disk, partition_id, offset, data, len);
}

static int hash_partition(
    h2_loader_package_t *package,
    uint32_t partition_id,
    uint64_t image_size,
    char out_sha256[H2_LOADER_SHA256_HEX_SIZE],
    int report_install_progress) {
    uint8_t fallback_buffer[H2_LOADER_VALIDATE_IO_BUF_SIZE];
    uint8_t *buffer = fallback_buffer;
    size_t buffer_size = sizeof(fallback_buffer);
    uint8_t digest[H2_LOADER_SHA256_SIZE];
    uint64_t offset = 0u;
    int rc;

    if (package == NULL || out_sha256 == NULL || image_size == 0u ||
        package->config.digest.start == NULL || package->config.digest.update == NULL ||
        package->config.digest.finish == NULL || package->config.digest.abort == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (package->config.allocator != NULL) {
        uint8_t *allocated = (uint8_t *)h2_pal_mem_alloc(
            package->config.allocator, H2_LOADER_IMAGE_IO_BUF_SIZE);
        if (allocated != NULL) {
            buffer = allocated;
            buffer_size = H2_LOADER_IMAGE_IO_BUF_SIZE;
        }
    }
    rc = package->config.digest.start(package->config.digest.user);
    if (rc != H2_PAL_OK) {
        if (buffer != fallback_buffer) {
            h2_pal_mem_free(package->config.allocator, buffer);
        }
        return rc;
    }
    while (offset < image_size) {
        size_t take = image_size - offset > buffer_size ?
            buffer_size : (size_t)(image_size - offset);
        rc = image_read(package, partition_id, offset, buffer, take);
        if (rc != H2_PAL_OK) {
            package->config.digest.abort(package->config.digest.user);
            if (buffer != fallback_buffer) {
                h2_pal_mem_free(package->config.allocator, buffer);
            }
            return rc;
        }
        rc = package->config.digest.update(package->config.digest.user, buffer, take);
        if (rc != H2_PAL_OK) {
            package->config.digest.abort(package->config.digest.user);
            if (buffer != fallback_buffer) {
                h2_pal_mem_free(package->config.allocator, buffer);
            }
            return rc;
        }
        offset += take;
        if (report_install_progress) {
            install_progress(package, H2_LOADER_INSTALL_PHASE_VERIFY,
                offset, image_size, "partition sha256");
        }
    }
    rc = package->config.digest.finish(package->config.digest.user, digest);
    package->config.digest.abort(package->config.digest.user);
    if (buffer != fallback_buffer) {
        h2_pal_mem_free(package->config.allocator, buffer);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    digest_to_hex(digest, out_sha256);
    return H2_PAL_OK;
}

static int read_pref_string(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    char *out,
    size_t out_len) {
    h2_pal_pref_namespace_t *ns = NULL;
    char *value = NULL;
    int rc;

    if (out == NULL || out_len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (pref == NULL || allocator == NULL || key == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    rc = h2_pal_pref_open(pref, H2_LOADER_PREF_NAMESPACE, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->get_string == NULL) {
        if (ns != NULL && ns->close != NULL) {
            (void)ns->close(ns);
        }
        return H2_PAL_ERR_UNSUPPORTED;
    }
    rc = ns->get_string(ns, allocator, key, &value);
    if (rc == H2_PAL_OK && value != NULL) {
        copy_text(out, out_len, value);
        h2_pal_mem_free(allocator, value);
    }
    if (ns->close != NULL) {
        (void)ns->close(ns);
    }
    return rc;
}

static int read_pref_u32(
    const h2_pal_pref_api_t *pref,
    const char *key,
    uint32_t *out_value) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc;
    int close_rc;

    if (pref == NULL || key == NULL || out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = 0u;
    rc = h2_pal_pref_open(pref, H2_LOADER_PREF_NAMESPACE, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->get_u32 == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->get_u32(ns, key, out_value);
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int layout_validator_start_entry(
    h2_loader_layout_validator_t *validator,
    const h2_bundle_entry_t *entry) {
    if (validator == NULL || entry == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    size_t current_index = validator->entry_index++;
    validator->entry = *entry;
    validator->remaining = entry->size;
    validator->padding_remaining = h2_bundle_tar_padding(entry->size);
    validator->reading_header = 0;
    validator->reading_padding = 0;

    if (strcmp(entry->path, "manifest") == 0) {
        if (current_index != 0u || validator->manifest_seen ||
            entry->kind != H2_BUNDLE_ENTRY_FILE || entry->size == 0u ||
            entry->size > sizeof(validator->manifest_data)) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        validator->manifest_seen = 1;
        return H2_BUNDLE_OK;
    }
    if (strcmp(entry->path, "checksum") == 0) {
        if (validator->checksum_seen || entry->kind != H2_BUNDLE_ENTRY_FILE ||
            entry->size == 0u || entry->size >= H2_LOADER_IDENTITY_TEXT_MAX) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        if ((!validator->manifest_seen && current_index != 0u) ||
            (validator->manifest_seen && current_index != 1u)) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        if (entry->size == 0u) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        return H2_BUNDLE_OK;
    }
    if ((strcmp(entry->path, "data") == 0 || strcmp(entry->path, "app") == 0) &&
        entry->kind == H2_BUNDLE_ENTRY_DIR) {
        if (strcmp(entry->path, "data") == 0) {
            validator->data_seen = 1;
            if (validator->manifest_seen && validator->inspection != NULL &&
                validator->inspection->manifest.role == H2_LOADER_IMAGE_ROLE_H2LOADER) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
        }
        validator->reading_header = 1;
        return H2_BUNDLE_OK;
    }
    if (path_starts_with(entry->path, "data/")) {
        const char *relative_path = entry->path + strlen("data/");
        static const uint8_t separator = 0u;

        if (!validator->checksum_seen) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        if (!is_safe_relative_path(relative_path)) {
            return H2_BUNDLE_ERR_UNSAFE_PATH;
        }
        validator->data_seen = 1;
        if (validator->manifest_seen && validator->inspection != NULL &&
            validator->inspection->manifest.role == H2_LOADER_IMAGE_ROLE_H2LOADER) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        if (entry->kind != H2_BUNDLE_ENTRY_FILE && entry->kind != H2_BUNDLE_ENTRY_DIR) {
            return H2_BUNDLE_ERR_UNSUPPORTED_ENTRY;
        }
        if (entry->kind == H2_BUNDLE_ENTRY_DIR && entry->size != 0u) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        if (entry->kind == H2_BUNDLE_ENTRY_FILE && validator->manifest_seen) {
            if (!validator->data_digest_started || validator->digest == NULL ||
                validator->digest->update == NULL ||
                (validator->last_data_path[0] != '\0' &&
                    strcmp(validator->last_data_path, entry->path) >= 0)) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
            if (validator->digest->update(
                    validator->digest->user,
                    (const uint8_t *)entry->path,
                    strlen(entry->path)) != H2_PAL_OK ||
                validator->digest->update(
                    validator->digest->user,
                    &separator,
                    sizeof(separator)) != H2_PAL_OK) {
                return H2_BUNDLE_ERR_IO;
            }
            copy_text(
                validator->last_data_path,
                sizeof(validator->last_data_path),
                entry->path);
            if (entry->size == 0u &&
                validator->digest->update(
                    validator->digest->user,
                    &separator,
                    sizeof(separator)) != H2_PAL_OK) {
                return H2_BUNDLE_ERR_IO;
            }
        }
        if (validator->inspection != NULL &&
            entry->kind == H2_BUNDLE_ENTRY_FILE) {
            uint64_t *total = path_has_suffix(entry->path, ".pixa")
                                  ? &validator->inspection->pixa_bytes
                                  : &validator->inspection->data_bytes;
            if (UINT64_MAX - *total < entry->size) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
            *total += entry->size;
        }
        if (entry->size == 0u) {
            validator->reading_header = 1;
        }
        return H2_BUNDLE_OK;
    }
    if (path_starts_with(entry->path, "app/")) {
        if (!validator->checksum_seen || validator->app_seen ||
            entry->kind != H2_BUNDLE_ENTRY_FILE) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        if (strcmp(entry->path, validator->app_entry_path) != 0) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        validator->app_seen = 1;
        if (entry->size == 0u) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        if (validator->inspection != NULL) {
            copy_text(
                validator->inspection->image_path,
                sizeof(validator->inspection->image_path),
                entry->path);
            if (validator->manifest_seen) {
                uint8_t data_digest[H2_LOADER_SHA256_SIZE];
                char data_sha256[H2_LOADER_SHA256_HEX_SIZE];

                if (validator->inspection->manifest.image_size != entry->size ||
                    validator->digest == NULL || validator->digest->start == NULL ||
                    validator->digest->update == NULL || validator->digest->finish == NULL ||
                    validator->digest->abort == NULL || !validator->data_digest_started) {
                    return H2_BUNDLE_ERR_LAYOUT;
                }
                if (validator->digest->finish(
                        validator->digest->user,
                        data_digest) != H2_PAL_OK) {
                    validator->digest->abort(validator->digest->user);
                    validator->data_digest_started = 0;
                    return H2_BUNDLE_ERR_IO;
                }
                validator->digest->abort(validator->digest->user);
                validator->data_digest_started = 0;
                digest_to_hex(data_digest, data_sha256);
                if (memcmp(
                        validator->checksum_data,
                        data_sha256,
                        H2_LOADER_SHA256_HEX_SIZE - 1u) != 0) {
                    return H2_BUNDLE_ERR_LAYOUT;
                }
                validator->data_digest_verified = 1;
                if (validator->digest->start(validator->digest->user) != H2_PAL_OK) {
                    return H2_BUNDLE_ERR_IO;
                }
                validator->image_digest_started = 1;
            } else {
                validator->inspection->legacy = 1;
                validator->inspection->manifest.role = H2_LOADER_IMAGE_ROLE_APP;
                validator->inspection->manifest.image_size = entry->size;
            }
        }
        return H2_BUNDLE_OK;
    }
    return H2_BUNDLE_ERR_LAYOUT;
}

static int layout_validator_finish_entry(h2_loader_layout_validator_t *validator) {
    int rc;

    if (validator == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    if (strcmp(validator->entry.path, "manifest") == 0) {
        if (validator->inspection == NULL || validator->manifest_len != validator->entry.size) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        rc = h2_loader_package_manifest_parse(
            validator->manifest_data,
            validator->manifest_len,
            &validator->inspection->manifest);
        if (rc != H2_PAL_OK) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
    } else if (strcmp(validator->entry.path, "checksum") == 0) {
        size_t digest_len = validator->checksum_len;

        if (digest_len > 0u && validator->checksum_data[digest_len - 1u] == (uint8_t)'\n') {
            digest_len -= 1u;
        }
        if (validator->manifest_seen &&
            !is_lower_sha256_bytes(validator->checksum_data, digest_len)) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        if (validator->inspection == NULL ||
            validator->checksum_len > sizeof(validator->inspection->data_checksum)) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
        memcpy(validator->inspection->data_checksum,
            validator->checksum_data,
            validator->checksum_len);
        validator->inspection->data_checksum_len = validator->checksum_len;
        if (validator->manifest_seen) {
            if (validator->digest == NULL || validator->digest->start == NULL ||
                validator->digest->update == NULL || validator->digest->finish == NULL ||
                validator->digest->abort == NULL ||
                validator->digest->start(validator->digest->user) != H2_PAL_OK) {
                return H2_BUNDLE_ERR_IO;
            }
            validator->data_digest_started = 1;
        }
        validator->checksum_seen = 1;
    } else if (path_starts_with(validator->entry.path, "data/") &&
        validator->entry.kind == H2_BUNDLE_ENTRY_FILE &&
        validator->data_digest_started) {
        static const uint8_t separator = 0u;
        if (validator->digest->update(
                validator->digest->user,
                &separator,
                sizeof(separator)) != H2_PAL_OK) {
            return H2_BUNDLE_ERR_IO;
        }
    } else if (path_starts_with(validator->entry.path, "app/") &&
        validator->image_digest_started) {
        uint8_t digest[H2_LOADER_SHA256_SIZE];
        char image_sha256[H2_LOADER_SHA256_HEX_SIZE];

        rc = validator->digest->finish(validator->digest->user, digest);
        validator->digest->abort(validator->digest->user);
        validator->image_digest_started = 0;
        if (rc != H2_PAL_OK) {
            return H2_BUNDLE_ERR_IO;
        }
        digest_to_hex(digest, image_sha256);
        if (strcmp(image_sha256, validator->inspection->manifest.image_sha256) != 0) {
            return H2_BUNDLE_ERR_LAYOUT;
        }
    }
    if (validator->padding_remaining > 0u) {
        validator->reading_padding = 1;
    } else {
        validator->reading_header = 1;
    }
    return H2_BUNDLE_OK;
}

static int layout_validator_consume_header(
    h2_loader_layout_validator_t *validator,
    const uint8_t **cursor,
    size_t *remaining) {
    size_t take;
    h2_bundle_entry_t entry;
    int header_result = 0;
    int rc;

    take = 512u - validator->header_len;
    if (take > *remaining) {
        take = *remaining;
    }
    memcpy(validator->header + validator->header_len, *cursor, take);
    validator->header_len += take;
    *cursor += take;
    *remaining -= take;
    if (validator->header_len < 512u) {
        return H2_BUNDLE_OK;
    }

    validator->header_len = 0u;
    rc = h2_bundle_tar_parse_header(validator->header, &entry, &header_result);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    if (header_result == H2_BUNDLE_TAR_HEADER_ZERO) {
        validator->zero_blocks += 1;
        if (validator->zero_blocks >= 2) {
            validator->done = 1;
        }
        return H2_BUNDLE_OK;
    }
    if (validator->done) {
        return H2_BUNDLE_ERR_LAYOUT;
    }
    validator->zero_blocks = 0;
    return layout_validator_start_entry(validator, &entry);
}

static int layout_validator_consume_file(
    h2_loader_layout_validator_t *validator,
    const uint8_t **cursor,
    size_t *remaining) {
    size_t take = *remaining;

    if ((uint64_t)take > validator->remaining) {
        take = (size_t)validator->remaining;
    }
    if (strcmp(validator->entry.path, "manifest") == 0) {
        if (validator->manifest_len > sizeof(validator->manifest_data) - take) {
            return H2_BUNDLE_ERR_NO_SPACE;
        }
        memcpy(validator->manifest_data + validator->manifest_len, *cursor, take);
        validator->manifest_len += take;
    } else if (strcmp(validator->entry.path, "checksum") == 0) {
        if (take > sizeof(validator->checksum_data) - validator->checksum_len) {
            return H2_BUNDLE_ERR_NO_SPACE;
        }
        memcpy(validator->checksum_data + validator->checksum_len, *cursor, take);
        validator->checksum_len += take;
    } else if (validator->data_digest_started &&
        path_starts_with(validator->entry.path, "data/") &&
        validator->entry.kind == H2_BUNDLE_ENTRY_FILE) {
        int rc = validator->digest->update(validator->digest->user, *cursor, take);
        if (rc != H2_PAL_OK) {
            return H2_BUNDLE_ERR_IO;
        }
    } else if (validator->image_digest_started &&
        path_starts_with(validator->entry.path, "app/")) {
        int rc = validator->digest->update(validator->digest->user, *cursor, take);
        if (rc != H2_PAL_OK) {
            validator->digest->abort(validator->digest->user);
            validator->image_digest_started = 0;
            return H2_BUNDLE_ERR_IO;
        }
    }
    validator->remaining -= take;
    *cursor += take;
    *remaining -= take;
    if (validator->remaining == 0u) {
        return layout_validator_finish_entry(validator);
    }
    return H2_BUNDLE_OK;
}

static int layout_validator_consume_padding(
    h2_loader_layout_validator_t *validator,
    const uint8_t **cursor,
    size_t *remaining) {
    size_t take = *remaining;

    if ((uint64_t)take > validator->padding_remaining) {
        take = (size_t)validator->padding_remaining;
    }
    validator->padding_remaining -= take;
    *cursor += take;
    *remaining -= take;
    if (validator->padding_remaining == 0u) {
        validator->reading_padding = 0;
        validator->reading_header = 1;
    }
    return H2_BUNDLE_OK;
}

static int layout_validator_feed(h2_loader_layout_validator_t *validator, const uint8_t *data, size_t len) {
    const uint8_t *cursor = data;
    size_t remaining = len;
    int rc;

    if (validator == NULL || (data == NULL && len != 0u)) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    if (validator->done) {
        for (size_t i = 0u; i < len; ++i) {
            if (data[i] != 0u) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
        }
        return H2_BUNDLE_OK;
    }
    while (remaining > 0u && !validator->done) {
        if (validator->reading_header) {
            rc = layout_validator_consume_header(validator, &cursor, &remaining);
        } else if (validator->reading_padding) {
            rc = layout_validator_consume_padding(validator, &cursor, &remaining);
        } else {
            rc = layout_validator_consume_file(validator, &cursor, &remaining);
        }
        if (rc != H2_BUNDLE_OK) {
            return rc;
        }
    }
    if (validator->done) {
        while (remaining > 0u) {
            if (*cursor++ != 0u) {
                return H2_BUNDLE_ERR_LAYOUT;
            }
            remaining -= 1u;
        }
    }
    return H2_BUNDLE_OK;
}

static int layout_validator_finish(const h2_loader_layout_validator_t *validator) {
    if (validator == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    if (!validator->done || !validator->checksum_seen || !validator->app_seen ||
        validator->header_len != 0u || !validator->reading_header) {
        return H2_BUNDLE_ERR_LAYOUT;
    }
    if (validator->manifest_seen && !validator->data_digest_verified) {
        return H2_BUNDLE_ERR_LAYOUT;
    }
    if (validator->manifest_seen && validator->inspection != NULL &&
        validator->inspection->manifest.role == H2_LOADER_IMAGE_ROLE_H2LOADER &&
        validator->data_seen) {
        return H2_BUNDLE_ERR_LAYOUT;
    }
    return H2_BUNDLE_OK;
}

int h2_loader_package_init(h2_loader_package_t *package, const h2_loader_package_config_t *config) {
    int rc;

    if (package == NULL || config == NULL || config->fs == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(package, 0, sizeof(*package));
    package->config = *config;
    package->config.package_path = default_if_empty(config->package_path, H2_LOADER_DEFAULT_PACKAGE_PATH);
    package->config.data_root = default_if_empty(config->data_root, H2_LOADER_DEFAULT_DATA_ROOT);
    package->config.installed_checksum_path =
        default_if_empty(config->installed_checksum_path, H2_LOADER_DEFAULT_CHECKSUM_PATH);
    package->config.app_entry_path = default_if_empty(config->app_entry_path, H2_LOADER_DEFAULT_APP_ENTRY_PATH);
    package->config.app_writer = config->app_writer;
    package->config.clear_data = config->clear_data;
    package->config.clear_data_user = config->clear_data_user;
    rc = h2_bundle_installer_init(&package->installer, package->config.fs, package->config.allocator);
    if (rc == H2_BUNDLE_OK) {
        h2_bundle_installer_set_progress(
            &package->installer, bundle_install_progress, package);
    }
    return rc == H2_BUNDLE_OK ? H2_PAL_OK : rc;
}

int h2_loader_package_recover_publish(
    const h2_pal_fs_api_t *fs,
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    const char *package_path,
    const char *previous_path) {
    h2_loader_metadata_t stage = {0};
    h2_pal_fs_stat_t package_stat;
    h2_pal_fs_stat_t previous_stat;
    h2_pal_fs_stat_t temporary_stat;
    char temporary_path[H2_BUNDLE_PATH_MAX];
    int package_exists = 0;
    int previous_exists = 0;
    int temporary_exists = 0;
    int stage_present = 0;
    int malformed_candidate = 0;
    int discard_candidate;
    int rc;

    if (fs == NULL || pref == NULL || allocator == NULL ||
        package_path == NULL || previous_path == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = snprintf(
        temporary_path, sizeof(temporary_path), "%s.tmp", package_path);
    if (rc < 0 || (size_t)rc >= sizeof(temporary_path)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    rc = h2_pal_fs_stat(fs, package_path, &package_stat);
    if (rc == H2_PAL_FS_OK) {
        if (package_stat.is_dir) {
            malformed_candidate = 1;
        }
        package_exists = 1;
    } else if (rc != H2_PAL_FS_ERR_NOT_FOUND) {
        return rc;
    }
    rc = h2_pal_fs_stat(fs, previous_path, &previous_stat);
    if (rc == H2_PAL_FS_OK) {
        if (previous_stat.is_dir) {
            malformed_candidate = 1;
        }
        previous_exists = 1;
    } else if (rc != H2_PAL_FS_ERR_NOT_FOUND) {
        return rc;
    }
    rc = h2_pal_fs_stat(fs, temporary_path, &temporary_stat);
    if (rc == H2_PAL_FS_OK) {
        if (temporary_stat.is_dir) {
            malformed_candidate = 1;
        }
        temporary_exists = 1;
    } else if (rc != H2_PAL_FS_ERR_NOT_FOUND) {
        return rc;
    }
    rc = h2_loader_metadata_read(
        pref,
        allocator,
        H2_LOADER_METADATA_SLOT_STAGE,
        &stage,
        &stage_present);
    if (rc != H2_PAL_OK) return rc;
    discard_candidate =
        malformed_candidate ||
        temporary_exists ||
        !package_exists ||
        !stage_present ||
        !stage.valid ||
        (package_exists && package_stat.size != stage.package_size);
    if (temporary_exists) {
        rc = h2_pal_fs_remove(fs, temporary_path);
        if (rc != H2_PAL_FS_OK && rc != H2_PAL_FS_ERR_NOT_FOUND) {
            return rc;
        }
    }
    if (previous_exists) {
        rc = h2_pal_fs_remove(fs, previous_path);
        if (rc != H2_PAL_FS_OK && rc != H2_PAL_FS_ERR_NOT_FOUND) {
            return rc;
        }
    }
    if (discard_candidate && package_exists) {
        rc = h2_pal_fs_remove(fs, package_path);
        if (rc != H2_PAL_FS_OK && rc != H2_PAL_FS_ERR_NOT_FOUND) {
            return rc;
        }
    }
    return discard_candidate ?
        h2_loader_metadata_clear(pref, H2_LOADER_METADATA_SLOT_STAGE) :
        H2_PAL_OK;
}

int h2_loader_package_read_staged_identity(
    h2_loader_package_t *package,
    const h2_pal_pref_api_t *pref,
    h2_loader_identity_t *out_identity) {
    h2_pal_fs_stat_t stat;
    uint32_t staged_size = 0u;
    int rc;

    if (package == NULL || out_identity == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_identity, 0, sizeof(*out_identity));
    if (package->config.fs == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    rc = h2_pal_fs_stat(package->config.fs, package->config.package_path, &stat);
    if (rc == H2_PAL_FS_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    if (stat.is_dir) {
        return H2_PAL_OK;
    }
    rc = read_pref_string(pref, package->config.allocator, "staged_checksum",
        out_identity->checksum, sizeof(out_identity->checksum));
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = read_pref_u32(pref, "staged_size", &staged_size);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (!is_sha256_hex(out_identity->checksum) || stat.size != (uint64_t)staged_size) {
        memset(out_identity, 0, sizeof(*out_identity));
        return H2_PAL_OK;
    }
    rc = read_pref_string(pref, package->config.allocator, "staged_version",
        out_identity->version, sizeof(out_identity->version));
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        copy_text(out_identity->version, sizeof(out_identity->version), out_identity->checksum);
    } else if (rc != H2_PAL_OK) {
        memset(out_identity, 0, sizeof(*out_identity));
        return rc;
    }
    out_identity->valid = 1;
    out_identity->size = stat.size;
    return H2_PAL_OK;
}

static void layout_validator_abort(h2_loader_layout_validator_t *validator) {
    if (validator != NULL &&
        (validator->image_digest_started || validator->data_digest_started) &&
        validator->digest != NULL && validator->digest->abort != NULL) {
        validator->digest->abort(validator->digest->user);
        validator->image_digest_started = 0;
        validator->data_digest_started = 0;
    }
}

int h2_loader_package_inspect_path(
    h2_loader_package_t *package,
    const char *archive_path,
    h2_loader_package_inspection_t *out_inspection) {
    uint8_t in[H2_LOADER_VALIDATE_IO_BUF_SIZE];
    uint8_t out[H2_LOADER_VALIDATE_IO_BUF_SIZE];
    h2_loader_layout_validator_t validator;
    h2_pal_fs_file_t *archive = NULL;
    z_stream stream;
    int stream_done = 0;
    int rc;

    if (package == NULL || package->config.fs == NULL || archive_path == NULL || out_inspection == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_inspection, 0, sizeof(*out_inspection));
    memset(&validator, 0, sizeof(validator));
    validator.app_entry_path = package->config.app_entry_path;
    validator.digest = &package->config.digest;
    validator.inspection = out_inspection;
    validator.reading_header = 1;

    rc = h2_pal_fs_open(package->config.fs, archive_path, H2_PAL_FS_OPEN_READ, &archive);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    memset(&stream, 0, sizeof(stream));
    if (package->config.allocator != NULL) {
        stream.zalloc = package_zlib_alloc;
        stream.zfree = package_zlib_free;
        stream.opaque = (voidpf)package->config.allocator;
    }
    rc = inflateInit(&stream);
    if (rc != Z_OK) {
        (void)h2_pal_fs_close(package->config.fs, archive);
        return H2_BUNDLE_ERR_ZLIB;
    }

    while (!stream_done) {
        size_t read_len = 0u;
        rc = h2_pal_fs_read(package->config.fs, archive, in, sizeof(in), &read_len);
        if (rc != H2_PAL_FS_OK) {
            layout_validator_abort(&validator);
            (void)inflateEnd(&stream);
            (void)h2_pal_fs_close(package->config.fs, archive);
            return rc;
        }
        if (read_len == 0u) {
            break;
        }
        stream.next_in = in;
        stream.avail_in = (uInt)read_len;
        while (stream.avail_in > 0u) {
            int zrc;
            stream.next_out = out;
            stream.avail_out = sizeof(out);
            zrc = inflate(&stream, Z_NO_FLUSH);
            if (zrc != Z_OK && zrc != Z_STREAM_END) {
                layout_validator_abort(&validator);
                (void)inflateEnd(&stream);
                (void)h2_pal_fs_close(package->config.fs, archive);
                return H2_BUNDLE_ERR_ZLIB;
            }
            rc = layout_validator_feed(&validator, out, sizeof(out) - stream.avail_out);
            if (rc != H2_BUNDLE_OK) {
                layout_validator_abort(&validator);
                (void)inflateEnd(&stream);
                (void)h2_pal_fs_close(package->config.fs, archive);
                return rc;
            }
            if (zrc == Z_STREAM_END) {
                stream_done = 1;
                break;
            }
        }
    }

    (void)inflateEnd(&stream);
    rc = h2_pal_fs_close(package->config.fs, archive);
    if (rc != H2_PAL_FS_OK) {
        layout_validator_abort(&validator);
        return rc;
    }
    if (!stream_done) {
        layout_validator_abort(&validator);
        return H2_BUNDLE_ERR_ZLIB;
    }
    rc = layout_validator_finish(&validator);
    layout_validator_abort(&validator);
    return rc;
}

int h2_loader_package_validate_path(h2_loader_package_t *package, const char *archive_path) {
    h2_loader_package_inspection_t inspection;
    return h2_loader_package_inspect_path(package, archive_path, &inspection);
}

int h2_loader_package_inspect(
    h2_loader_package_t *package,
    const h2_pal_pref_api_t *pref,
    h2_loader_package_inspection_t *out_inspection) {
    h2_loader_identity_t staged;
    int rc;

    if (package == NULL || pref == NULL || out_inspection == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_loader_package_read_staged_identity(package, pref, &staged);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (!staged.valid) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    rc = verify_archive_checksum(package, &staged);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_loader_package_inspect_path(package, package->config.package_path, out_inspection);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    out_inspection->staged = staged;
    return H2_PAL_OK;
}

static int installed_data_checksum_matches(
    h2_loader_package_t *package,
    const h2_loader_package_inspection_t *inspection,
    int *out_matches) {
    h2_pal_fs_file_t *file = NULL;
    uint8_t actual[H2_LOADER_IDENTITY_TEXT_MAX];
    size_t total = 0u;
    size_t extra_len = 0u;
    uint8_t extra = 0u;
    int rc;
    int close_rc;

    if (package == NULL || inspection == NULL || out_matches == NULL ||
        inspection->data_checksum_len == 0u ||
        inspection->data_checksum_len > sizeof(actual)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_matches = 0;
    rc = h2_pal_fs_open(
        package->config.fs,
        package->config.installed_checksum_path,
        H2_PAL_FS_OPEN_READ,
        &file);
    if (rc == H2_PAL_FS_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    while (total < inspection->data_checksum_len) {
        size_t read_len = 0u;
        rc = h2_pal_fs_read(
            package->config.fs,
            file,
            actual + total,
            inspection->data_checksum_len - total,
            &read_len);
        if (rc != H2_PAL_FS_OK || read_len == 0u) {
            break;
        }
        total += read_len;
    }
    if (rc == H2_PAL_FS_OK && total == inspection->data_checksum_len) {
        rc = h2_pal_fs_read(package->config.fs, file, &extra, sizeof(extra), &extra_len);
    }
    close_rc = h2_pal_fs_close(package->config.fs, file);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    if (close_rc != H2_PAL_FS_OK) {
        return close_rc;
    }
    *out_matches = total == inspection->data_checksum_len && extra_len == 0u &&
        memcmp(actual, inspection->data_checksum, inspection->data_checksum_len) == 0;
    return H2_PAL_OK;
}

int h2_loader_package_plan_install(
    h2_loader_package_t *package,
    const h2_loader_package_inspection_t *inspection,
    uint32_t destination_partition_id,
    h2_loader_package_install_plan_t *out_plan) {
    int matches = 0;
    int rc;

    if (package == NULL || inspection == NULL || out_plan == NULL ||
        destination_partition_id == 0u ||
        inspection->data_checksum_len == 0u ||
        inspection->data_checksum_len > sizeof(inspection->data_checksum) ||
        (inspection->manifest.role != H2_LOADER_IMAGE_ROLE_APP &&
            inspection->manifest.role != H2_LOADER_IMAGE_ROLE_H2LOADER)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_plan, 0, sizeof(*out_plan));
    out_plan->update_app = 1;
    if (inspection->manifest.role == H2_LOADER_IMAGE_ROLE_H2LOADER) {
        return H2_PAL_OK;
    }
    if (!inspection->legacy &&
        (package->config.image_reader != NULL || package->config.disk != NULL)) {
        rc = h2_loader_image_verify(
            package,
            &inspection->manifest,
            destination_partition_id);
        if (rc == H2_PAL_OK) {
            out_plan->update_app = 0;
        } else if (rc != H2_PAL_ERR_FORMAT) {
            return rc;
        }
    }
    rc = installed_data_checksum_matches(package, inspection, &matches);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    out_plan->update_data = !matches;
    return H2_PAL_OK;
}

int h2_loader_package_install_staged(
    h2_loader_package_t *package,
    const h2_loader_identity_t *identity) {
    h2_loader_disk_writer_t disk_writer;
    h2_bundle_app_writer_t app_writer;
    const h2_bundle_app_writer_t *selected_app_writer;
    h2_bundle_ota_options_t options;

    if (package == NULL || identity == NULL || !identity->valid) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    {
        int rc = verify_archive_checksum(package, identity);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    memset(&disk_writer, 0, sizeof(disk_writer));
    memset(&app_writer, 0, sizeof(app_writer));
    selected_app_writer = package->config.app_writer;
    if (selected_app_writer == NULL) {
        if (package->config.disk == NULL) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        disk_writer.disk = package->config.disk;
        disk_writer.app_entry_path = package->config.app_entry_path;
        disk_writer.package = package;
        disk_writer.partition_id = package->config.app_partition_id;

        app_writer.user = &disk_writer;
        app_writer.begin = disk_writer_begin;
        app_writer.write = disk_writer_write;
        app_writer.end = disk_writer_end;
        selected_app_writer = &app_writer;
    }

    memset(&options, 0, sizeof(options));
    options.archive_path = package->config.package_path;
    options.data_root = package->config.data_root;
    options.installed_checksum_path = package->config.installed_checksum_path;
    options.app_writer = selected_app_writer;
    options.clear_data = package->config.clear_data;
    options.clear_data_user = package->config.clear_data_user;
    return h2_bundle_install_ota(&package->installer, &options);
}

int h2_loader_package_install_to(
    h2_loader_package_t *package,
    const h2_loader_package_inspection_t *inspection,
    uint32_t destination_partition_id,
    const h2_loader_package_install_plan_t *plan,
    h2_loader_package_install_result_t *out_result) {
    h2_loader_disk_writer_t disk_writer;
    h2_loader_writer_bridge_t bridge;
    h2_bundle_app_writer_t app_writer;
    h2_bundle_ota_options_t options;
    h2_loader_package_inspection_t fresh;
    uint64_t capacity;
    char destination_sha256[H2_LOADER_SHA256_HEX_SIZE];
    int rc;

    if (out_result != NULL) {
        memset(out_result, 0, sizeof(*out_result));
    }
    if (package == NULL || inspection == NULL || plan == NULL || out_result == NULL ||
        !inspection->staged.valid ||
        inspection->data_checksum_len == 0u ||
        inspection->data_checksum_len > sizeof(inspection->data_checksum) ||
        (plan->update_app != 0 && plan->update_app != 1) ||
        (plan->update_data != 0 && plan->update_data != 1) ||
        destination_partition_id == 0u || inspection->manifest.image_size == 0u ||
        (inspection->manifest.role != H2_LOADER_IMAGE_ROLE_APP &&
            inspection->manifest.role != H2_LOADER_IMAGE_ROLE_H2LOADER) ||
        (inspection->manifest.role == H2_LOADER_IMAGE_ROLE_H2LOADER &&
            (!plan->update_app || plan->update_data))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    install_progress(package, H2_LOADER_INSTALL_PHASE_VALIDATE, 0u, 1u,
        "package");
    rc = image_capacity(package, destination_partition_id, &capacity);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (capacity > 0u && inspection->manifest.image_size > capacity) {
        return H2_PAL_ERR_NO_SPACE;
    }
    rc = verify_archive_checksum(package, &inspection->staged);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_loader_package_inspect_path(package, package->config.package_path, &fresh);
    if (rc != H2_PAL_OK || fresh.legacy != inspection->legacy ||
        fresh.manifest.role != inspection->manifest.role ||
        strcmp(fresh.manifest.board, inspection->manifest.board) != 0 ||
        strcmp(fresh.manifest.target, inspection->manifest.target) != 0 ||
        strcmp(fresh.manifest.version, inspection->manifest.version) != 0 ||
        fresh.manifest.image_size != inspection->manifest.image_size ||
        strcmp(fresh.manifest.image_sha256, inspection->manifest.image_sha256) != 0 ||
        strcmp(fresh.image_path, inspection->image_path) != 0 ||
        fresh.data_checksum_len != inspection->data_checksum_len ||
        memcmp(fresh.data_checksum,
            inspection->data_checksum,
            inspection->data_checksum_len) != 0) {
        return rc == H2_PAL_OK ? H2_PAL_ERR_FORMAT : rc;
    }
    install_progress(package, H2_LOADER_INSTALL_PHASE_VALIDATE, 1u, 1u,
        "package");

    memset(&disk_writer, 0, sizeof(disk_writer));
    memset(&bridge, 0, sizeof(bridge));
    memset(&app_writer, 0, sizeof(app_writer));
    package->bundle_progress_bytes = 0u;
    package->data_progress_bytes = 0u;
    package->data_total_bytes = plan->update_data ? inspection->data_bytes : 0u;
    package->pixa_progress_bytes = 0u;
    package->pixa_total_bytes = plan->update_data ? inspection->pixa_bytes : 0u;
    if (plan->update_app && package->config.image_writer != NULL) {
        bridge.api = package->config.image_writer;
        bridge.identity = &inspection->manifest;
        bridge.partition_id = destination_partition_id;
        bridge.package = package;
        app_writer.user = &bridge;
        app_writer.begin = writer_bridge_begin;
        app_writer.write = writer_bridge_write;
        app_writer.end = writer_bridge_end;
        app_writer.abort = writer_bridge_abort;
    } else if (plan->update_app) {
        if (package->config.disk == NULL) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        disk_writer.disk = package->config.disk;
        disk_writer.app_entry_path = package->config.app_entry_path;
        disk_writer.package = package;
        disk_writer.partition_id = destination_partition_id;
        app_writer.user = &disk_writer;
        app_writer.begin = disk_writer_begin;
        app_writer.write = disk_writer_write;
        app_writer.end = disk_writer_end;
    }

    memset(&options, 0, sizeof(options));
    options.archive_path = package->config.package_path;
    options.data_root = package->config.data_root;
    options.installed_checksum_path = package->config.installed_checksum_path;
    options.app_writer = plan->update_app ? &app_writer : NULL;
    options.clear_data = package->config.clear_data;
    options.clear_data_user = package->config.clear_data_user;
    options.skip_app_install = !plan->update_app;
    options.skip_data_install = !plan->update_data;
    rc = h2_bundle_install_ota(&package->installer, &options);
    if (rc != H2_BUNDLE_OK) {
        return rc;
    }
    if (!plan->update_app) {
        install_progress(package, H2_LOADER_INSTALL_PHASE_IMAGE, 1u, 1u,
            "unchanged");
    }
    if (!plan->update_data) {
        const char *detail = inspection->manifest.role == H2_LOADER_IMAGE_ROLE_H2LOADER
            ? "skipped for loader" : "unchanged";
        install_progress(package, H2_LOADER_INSTALL_PHASE_DATA, 1u, 1u,
            detail);
        install_progress(package, H2_LOADER_INSTALL_PHASE_PIXA, 1u, 1u,
            detail);
    } else {
        install_progress(
            package, H2_LOADER_INSTALL_PHASE_DATA,
            package->data_total_bytes, package->data_total_bytes,
            "installed");
        install_progress(
            package, H2_LOADER_INSTALL_PHASE_PIXA,
            package->pixa_total_bytes, package->pixa_total_bytes,
            "expanded");
    }
    if (plan->update_app && !inspection->legacy &&
        (package->config.image_reader != NULL || package->config.disk != NULL)) {
        rc = hash_partition(
            package,
            destination_partition_id,
            inspection->manifest.image_size,
            destination_sha256,
            1);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (strcmp(destination_sha256, inspection->manifest.image_sha256) != 0) {
            return H2_PAL_ERR_FORMAT;
        }
    }
    install_progress(package, H2_LOADER_INSTALL_PHASE_DONE, 1u, 1u,
        plan->update_app || plan->update_data ? "installed" : "unchanged");
    out_result->app_written = plan->update_app;
    out_result->data_written = plan->update_data;
    return H2_PAL_OK;
}

int h2_loader_image_copy_to(
    h2_loader_package_t *package,
    const h2_loader_image_identity_t *identity,
    uint32_t source_partition_id,
    uint32_t destination_partition_id) {
    uint8_t fallback_buffer[H2_LOADER_VALIDATE_IO_BUF_SIZE];
    uint8_t *buffer = fallback_buffer;
    size_t buffer_size = sizeof(fallback_buffer);
    h2_loader_disk_writer_t disk_writer;
    h2_bundle_entry_t entry;
    uint64_t source_capacity;
    uint64_t destination_capacity;
    uint64_t offset = 0u;
    char sha256[H2_LOADER_SHA256_HEX_SIZE];
    int custom_writer_active = 0;
    int rc;

    if (package == NULL || identity == NULL || identity->image_size == 0u ||
        source_partition_id == 0u || destination_partition_id == 0u ||
        source_partition_id == destination_partition_id ||
        !is_lower_sha256_hex(identity->image_sha256)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = image_capacity(package, source_partition_id, &source_capacity);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = image_capacity(package, destination_partition_id, &destination_capacity);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((source_capacity > 0u && identity->image_size > source_capacity) ||
        (destination_capacity > 0u && identity->image_size > destination_capacity)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    if (package->config.allocator != NULL) {
        uint8_t *allocated = (uint8_t *)h2_pal_mem_alloc(
            package->config.allocator, H2_LOADER_IMAGE_IO_BUF_SIZE);
        if (allocated != NULL) {
            buffer = allocated;
            buffer_size = H2_LOADER_IMAGE_IO_BUF_SIZE;
        }
    }
    memset(&disk_writer, 0, sizeof(disk_writer));
    memset(&entry, 0, sizeof(entry));
    copy_text(entry.path, sizeof(entry.path), package->config.app_entry_path);
    entry.kind = H2_BUNDLE_ENTRY_FILE;
    entry.size = identity->image_size;
    if (package->config.image_writer != NULL && package->config.image_writer->vtable != NULL &&
        package->config.image_writer->vtable->begin != NULL &&
        package->config.image_writer->vtable->write != NULL &&
        package->config.image_writer->vtable->finish != NULL) {
        rc = package->config.image_writer->vtable->begin(
            package->config.image_writer->user,
            destination_partition_id,
            identity);
        if (rc != H2_PAL_OK) {
            goto cleanup;
        }
        custom_writer_active = 1;
        install_progress(package, H2_LOADER_INSTALL_PHASE_IMAGE, 0u,
            identity->image_size, "canonical image");
    } else {
        disk_writer.disk = package->config.disk;
        disk_writer.app_entry_path = package->config.app_entry_path;
        disk_writer.package = package;
        disk_writer.partition_id = destination_partition_id;
        rc = disk_writer_begin(&disk_writer, &entry);
        if (rc != H2_PAL_OK) {
            goto cleanup;
        }
    }
    while (offset < identity->image_size) {
        size_t take = identity->image_size - offset > buffer_size ?
            buffer_size : (size_t)(identity->image_size - offset);
        rc = image_read(package, source_partition_id, offset, buffer, take);
        if (rc == H2_PAL_OK) {
            rc = custom_writer_active ?
                package->config.image_writer->vtable->write(
                    package->config.image_writer->user,
                    buffer,
                    take) :
                disk_writer_write(&disk_writer, &entry, buffer, take);
        }
        if (rc != H2_PAL_OK) {
            if (custom_writer_active && package->config.image_writer->vtable->abort != NULL) {
                package->config.image_writer->vtable->abort(package->config.image_writer->user);
            }
            goto cleanup;
        }
        offset += take;
        if (custom_writer_active) {
            install_progress(package, H2_LOADER_INSTALL_PHASE_IMAGE, offset,
                identity->image_size, "canonical image");
        }
    }
    rc = custom_writer_active ?
        package->config.image_writer->vtable->finish(package->config.image_writer->user, identity) :
        disk_writer_end(&disk_writer, &entry);
    if (rc != H2_PAL_OK) {
        if (custom_writer_active && package->config.image_writer->vtable->abort != NULL) {
            package->config.image_writer->vtable->abort(package->config.image_writer->user);
        }
        goto cleanup;
    }
    rc = hash_partition(package, destination_partition_id, identity->image_size,
        sha256, 1);
    if (rc != H2_PAL_OK) {
        goto cleanup;
    }
    rc = strcmp(sha256, identity->image_sha256) == 0 ?
        H2_PAL_OK : H2_PAL_ERR_FORMAT;

cleanup:
    if (buffer != fallback_buffer) {
        h2_pal_mem_free(package->config.allocator, buffer);
    }
    return rc;
}

int h2_loader_image_verify(
    h2_loader_package_t *package,
    const h2_loader_image_identity_t *identity,
    uint32_t partition_id) {
    uint64_t capacity;
    char sha256[H2_LOADER_SHA256_HEX_SIZE];
    int rc;

    if (package == NULL || identity == NULL || partition_id == 0u ||
        identity->image_size == 0u || !is_lower_sha256_hex(identity->image_sha256)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = image_capacity(package, partition_id, &capacity);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (capacity > 0u && identity->image_size > capacity) {
        return H2_PAL_ERR_NO_SPACE;
    }
    rc = hash_partition(package, partition_id, identity->image_size, sha256, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return strcmp(sha256, identity->image_sha256) == 0 ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

int h2_loader_package_identity_equal(
    const h2_loader_identity_t *a,
    const h2_loader_identity_t *b) {
    if (a == NULL || b == NULL || !a->valid || !b->valid) {
        return 0;
    }
    if (a->checksum[0] != '\0' || b->checksum[0] != '\0') {
        return strcmp(a->checksum, b->checksum) == 0;
    }
    if (a->version[0] != '\0' || b->version[0] != '\0') {
        return strcmp(a->version, b->version) == 0;
    }
    return a->size == b->size;
}
