#include "h2_h2loader_host_factory.h"

#include "h2_h2loader_host_internal.h"

#include <string.h>

#define FACTORY_PREAMBLE_SIZE 20u
#define FACTORY_RECORD_SIZE 180u
#define FACTORY_BOARD_OFFSET 20u
#define FACTORY_TARGET_OFFSET \
    (FACTORY_BOARD_OFFSET + H2_H2LOADER_HOST_IDENTITY_MAX_LEN)
#define FACTORY_RECORDS_OFFSET \
    (FACTORY_TARGET_OFFSET + H2_H2LOADER_HOST_IDENTITY_MAX_LEN)

static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
        ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static uint64_t read_le64(const uint8_t *data) {
    uint64_t value = 0u;
    for (size_t i = 0u; i < 8u; ++i) {
        value |= (uint64_t)data[i] << (i * 8u);
    }
    return value;
}

static h2_pal_result_t read_exact(
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    uint64_t offset,
    uint8_t *out,
    size_t len) {
    size_t consumed = 0u;
    while (consumed < len) {
        size_t read = 0u;
        h2_pal_result_t rc = read_payload(
            payload_user,
            offset + consumed,
            &out[consumed],
            len - consumed,
            &read);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (read == 0u || read > len - consumed) {
            return H2_PAL_ERR_TRUNCATED;
        }
        consumed += read;
    }
    return H2_PAL_OK;
}

static int copy_fixed_text(
    char *out,
    size_t out_size,
    const uint8_t *data,
    size_t data_size) {
    size_t len = 0u;
    while (len < data_size && data[len] != '\0') {
        ++len;
    }
    if (len == 0u || len >= data_size) {
        return 0;
    }
    for (size_t i = len + 1u; i < data_size; ++i) {
        if (data[i] != '\0') {
            return 0;
        }
    }
    return h2_h2loader_host_copy_text(
        out, out_size, (const char *)data, len);
}

static h2_pal_result_t verify_member(
    const h2_h2loader_host_factory_file_t *file,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user) {
    h2_h2loader_host_sha256_t sha;
    uint8_t buffer[8192];
    uint8_t digest[32];
    char actual[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
    uint64_t offset = 0u;

    h2_h2loader_host_sha256_init(&sha);
    while (offset < file->bytes) {
        size_t request =
            file->bytes - offset > sizeof(buffer)
                ? sizeof(buffer)
                : (size_t)(file->bytes - offset);
        h2_pal_result_t rc = read_exact(
            read_payload,
            payload_user,
            file->data_offset + offset,
            buffer,
            request);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        h2_h2loader_host_sha256_update(&sha, buffer, request);
        offset += request;
    }
    h2_h2loader_host_sha256_finish(&sha, digest);
    h2_h2loader_host_sha256_hex(digest, actual);
    return strcmp(actual, file->sha256) == 0
        ? H2_PAL_OK
        : H2_PAL_ERR_FORMAT;
}

h2_pal_result_t h2_h2loader_host_factory_open(
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_factory_manifest_t *out_manifest) {
    uint8_t header[H2_H2LOADER_HOST_FACTORY_HEADER_SIZE];
    uint64_t expected_data_offset =
        H2_H2LOADER_HOST_FACTORY_HEADER_SIZE;

    if (out_manifest != NULL) {
        memset(out_manifest, 0, sizeof(*out_manifest));
    }
    if (asset == NULL || read_payload == NULL || out_manifest == NULL ||
        asset->operation != H2_H2LOADER_HOST_ASSET_OPERATION_RECOVERY ||
        asset->role != H2_H2LOADER_HOST_ASSET_ROLE_LOADER ||
        asset->bytes <= H2_H2LOADER_HOST_FACTORY_HEADER_SIZE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = read_exact(
        read_payload,
        payload_user,
        0u,
        header,
        sizeof(header));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    uint16_t version = read_le16(&header[4]);
    uint16_t driver = read_le16(&header[6]);
    uint32_t file_count = read_le32(&header[16]);
    if (memcmp(header, "H2FB", 4u) != 0 || version != 1u ||
        (driver != H2_H2LOADER_HOST_FACTORY_DRIVER_ESP_ROM &&
         driver != H2_H2LOADER_HOST_FACTORY_DRIVER_BK7258_ROM) ||
        file_count == 0u ||
        file_count > H2_H2LOADER_HOST_FACTORY_FILE_CAPACITY) {
        return H2_PAL_ERR_FORMAT;
    }
    out_manifest->driver = (h2_h2loader_host_factory_driver_t)driver;
    out_manifest->flags = read_le32(&header[8]);
    out_manifest->baud = read_le32(&header[12]);
    out_manifest->file_count = file_count;
    if (out_manifest->flags != 1u || out_manifest->baud == 0u ||
        !copy_fixed_text(
            out_manifest->board,
            sizeof(out_manifest->board),
            &header[FACTORY_BOARD_OFFSET],
            H2_H2LOADER_HOST_IDENTITY_MAX_LEN) ||
        !copy_fixed_text(
            out_manifest->target,
            sizeof(out_manifest->target),
            &header[FACTORY_TARGET_OFFSET],
            H2_H2LOADER_HOST_IDENTITY_MAX_LEN) ||
        !h2_h2loader_host_is_safe_identity(out_manifest->board) ||
        !h2_h2loader_host_is_safe_identity(out_manifest->target) ||
        strcmp(out_manifest->board, asset->board) != 0 ||
        strcmp(out_manifest->target, asset->target) != 0 ||
        (out_manifest->driver ==
             H2_H2LOADER_HOST_FACTORY_DRIVER_ESP_ROM &&
         strncmp(out_manifest->target, "esp32", 5u) != 0) ||
        (out_manifest->driver ==
             H2_H2LOADER_HOST_FACTORY_DRIVER_BK7258_ROM &&
         strcmp(out_manifest->target, "bk7258") != 0)) {
        memset(out_manifest, 0, sizeof(*out_manifest));
        return H2_PAL_ERR_FORMAT;
    }
    for (size_t i = 0u; i < file_count; ++i) {
        const uint8_t *record =
            &header[FACTORY_RECORDS_OFFSET + i * FACTORY_RECORD_SIZE];
        h2_h2loader_host_factory_file_t *file =
            &out_manifest->files[i];
        file->flash_offset = read_le32(record);
        file->data_offset = read_le64(&record[4]);
        file->bytes = read_le64(&record[12]);
        h2_h2loader_host_sha256_hex(&record[20], file->sha256);
        if (!copy_fixed_text(
                file->name,
                sizeof(file->name),
                &record[52],
                H2_H2LOADER_HOST_FACTORY_NAME_MAX_LEN) ||
            !h2_h2loader_host_is_safe_resource_name(file->name) ||
            strchr(file->name, '/') != NULL ||
            file->data_offset != expected_data_offset ||
            file->data_offset > asset->bytes ||
            file->bytes == 0u ||
            file->bytes > asset->bytes - file->data_offset ||
            file->bytes > UINT32_MAX ||
            file->bytes > (uint64_t)UINT32_MAX + 1u -
                file->flash_offset ||
            (out_manifest->driver ==
                 H2_H2LOADER_HOST_FACTORY_DRIVER_ESP_ROM &&
             ((file->flash_offset % 4u) != 0u ||
              (file->bytes % 4u) != 0u))) {
            memset(out_manifest, 0, sizeof(*out_manifest));
            return H2_PAL_ERR_FORMAT;
        }
        for (size_t j = 0u; j < i; ++j) {
            uint64_t file_end =
                (uint64_t)file->flash_offset + file->bytes;
            uint64_t previous_end =
                (uint64_t)out_manifest->files[j].flash_offset +
                out_manifest->files[j].bytes;
            if (strcmp(
                    file->name,
                    out_manifest->files[j].name) == 0 ||
                ((uint64_t)file->flash_offset < previous_end &&
                 (uint64_t)out_manifest->files[j].flash_offset <
                     file_end)) {
                memset(out_manifest, 0, sizeof(*out_manifest));
                return H2_PAL_ERR_FORMAT;
            }
        }
        expected_data_offset += file->bytes;
        rc = verify_member(file, read_payload, payload_user);
        if (rc != H2_PAL_OK) {
            memset(out_manifest, 0, sizeof(*out_manifest));
            return rc;
        }
    }
    if (expected_data_offset != asset->bytes) {
        memset(out_manifest, 0, sizeof(*out_manifest));
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_factory_read_member(
    const h2_h2loader_host_factory_manifest_t *manifest,
    size_t file_index,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    uint64_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_read) {
    if (out_read != NULL) {
        *out_read = 0u;
    }
    if (manifest == NULL || read_payload == NULL ||
        (out_size > 0u && out == NULL) || out_read == NULL ||
        file_index >= manifest->file_count) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_h2loader_host_factory_file_t *file =
        &manifest->files[file_index];
    if (offset > file->bytes) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t request = file->bytes - offset > out_size
        ? out_size
        : (size_t)(file->bytes - offset);
    if (request == 0u) {
        return H2_PAL_OK;
    }
    return read_payload(
        payload_user,
        file->data_offset + offset,
        out,
        request,
        out_read);
}
