#include "h2_h2loader_host_package.h"

#include "h2_h2loader_host_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define TEST_TAR_CAPACITY 32768u
#define TEST_PACKAGE_CAPACITY 40000u

typedef struct test_buffer {
    uint8_t bytes[TEST_PACKAGE_CAPACITY];
    size_t len;
} test_buffer_t;

typedef struct package_options {
    const char *role;
    const char *first_data_path;
    const char *second_data_path;
    const char *checksum_override;
    const char *image_sha_override;
    const char *image_size_override;
    const char *version_override;
    const char *app_path;
} package_options_t;

static void *test_alloc(void *user, size_t size) {
    (void)user;
    return malloc(size);
}

static void test_free(void *user, void *address) {
    (void)user;
    free(address);
}

static const h2_pal_mem_vtable_t test_mem_vtable = {
    .alloc = test_alloc,
    .free = test_free,
};

static const h2_pal_mem_api_t test_mem = {
    .user = NULL,
    .vtable = &test_mem_vtable,
};

static void sha_hex(
    const uint8_t *data,
    size_t len,
    char out[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u]) {
    h2_h2loader_host_sha256_t sha;
    uint8_t digest[32];

    h2_h2loader_host_sha256_init(&sha);
    h2_h2loader_host_sha256_update(&sha, data, len);
    h2_h2loader_host_sha256_finish(&sha, digest);
    h2_h2loader_host_sha256_hex(digest, out);
}

static size_t append_tar_file(
    uint8_t *tar,
    size_t capacity,
    size_t offset,
    const char *path,
    const void *data,
    size_t len) {
    uint8_t *header;
    uint64_t sum = 0u;
    size_t padded_len = (len + 511u) & ~(size_t)511u;

    assert(tar != NULL && path != NULL && strlen(path) < 100u);
    assert(offset <= capacity && 512u + padded_len <= capacity - offset);
    header = tar + offset;
    memset(header, 0, 512u + padded_len);
    memcpy(header, path, strlen(path));
    (void)snprintf(
        (char *)header + 124u,
        12u,
        "%011llo",
        (unsigned long long)len);
    memset(header + 148u, ' ', 8u);
    header[156u] = '0';
    for (size_t i = 0u; i < 512u; ++i) {
        sum += header[i];
    }
    (void)snprintf(
        (char *)header + 148u,
        8u,
        "%06llo",
        (unsigned long long)sum);
    header[154u] = '\0';
    header[155u] = ' ';
    if (len > 0u) {
        assert(data != NULL);
        memcpy(header + 512u, data, len);
    }
    return offset + 512u + padded_len;
}

static void update_data_digest(
    h2_h2loader_host_sha256_t *sha,
    const char *path,
    const uint8_t *data,
    size_t len) {
    static const uint8_t separator = 0u;
    h2_h2loader_host_sha256_update(
        sha, (const uint8_t *)path, strlen(path));
    h2_h2loader_host_sha256_update(sha, &separator, sizeof(separator));
    h2_h2loader_host_sha256_update(sha, data, len);
    h2_h2loader_host_sha256_update(sha, &separator, sizeof(separator));
}

static test_buffer_t build_package(const package_options_t *options) {
    static const uint8_t app[] = "batch-loader-image";
    static const uint8_t first_data[] = "first";
    static const uint8_t second_data[] = "second";
    uint8_t tar[TEST_TAR_CAPACITY];
    char manifest[512];
    char image_sha[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
    char data_sha[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
    char checksum[H2_H2LOADER_HOST_SHA256_HEX_LEN + 2u];
    char image_size[32];
    h2_h2loader_host_sha256_t data_digest;
    uint8_t digest[32];
    size_t tar_len = 0u;
    test_buffer_t package = {0};
    uLongf compressed_len = sizeof(package.bytes);
    const char *role = options->role != NULL ? options->role : "app";
    const char *app_path = options->app_path != NULL
        ? options->app_path
        : "app/esp/app.bin";

    (void)snprintf(
        image_size,
        sizeof(image_size),
        "%llu",
        (unsigned long long)(sizeof(app) - 1u));

    sha_hex(app, sizeof(app) - 1u, image_sha);
    h2_h2loader_host_sha256_init(&data_digest);
    if (options->first_data_path != NULL) {
        update_data_digest(
            &data_digest,
            options->first_data_path,
            first_data,
            sizeof(first_data) - 1u);
    }
    if (options->second_data_path != NULL) {
        update_data_digest(
            &data_digest,
            options->second_data_path,
            second_data,
            sizeof(second_data) - 1u);
    }
    h2_h2loader_host_sha256_finish(&data_digest, digest);
    h2_h2loader_host_sha256_hex(digest, data_sha);
    (void)snprintf(
        checksum,
        sizeof(checksum),
        "%s\n",
        options->checksum_override != NULL
            ? options->checksum_override
            : data_sha);
    (void)snprintf(
        manifest,
        sizeof(manifest),
        "format=1\n"
        "role=%s\n"
        "board=devkit\n"
        "target=esp32s3\n"
        "version=%s\n"
        "image_size=%s\n"
        "image_sha256=%s\n",
        role,
        options->version_override != NULL
            ? options->version_override
            : "v1.2.3",
        options->image_size_override != NULL
            ? options->image_size_override
            : image_size,
        options->image_sha_override != NULL
            ? options->image_sha_override
            : image_sha);

    memset(tar, 0, sizeof(tar));
    tar_len = append_tar_file(
        tar, sizeof(tar), tar_len, "manifest", manifest, strlen(manifest));
    tar_len = append_tar_file(
        tar, sizeof(tar), tar_len, "checksum", checksum, strlen(checksum));
    if (options->first_data_path != NULL) {
        tar_len = append_tar_file(
            tar,
            sizeof(tar),
            tar_len,
            options->first_data_path,
            first_data,
            sizeof(first_data) - 1u);
    }
    if (options->second_data_path != NULL) {
        tar_len = append_tar_file(
            tar,
            sizeof(tar),
            tar_len,
            options->second_data_path,
            second_data,
            sizeof(second_data) - 1u);
    }
    tar_len = append_tar_file(
        tar,
        sizeof(tar),
        tar_len,
        app_path,
        app,
        sizeof(app) - 1u);
    assert(tar_len + 1024u <= sizeof(tar));
    tar_len += 1024u;
    assert(compress2(
               package.bytes,
               &compressed_len,
               tar,
               (uLong)tar_len,
               Z_BEST_SPEED) == Z_OK);
    package.len = (size_t)compressed_len;
    return package;
}

static h2_pal_result_t read_buffer(
    void *user,
    uint64_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_read) {
    const test_buffer_t *buffer = (const test_buffer_t *)user;
    size_t available;
    size_t take;

    if (buffer == NULL || out == NULL || out_read == NULL ||
        offset > buffer->len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    available = buffer->len - (size_t)offset;
    take = available < out_size ? available : out_size;
    memcpy(out, buffer->bytes + (size_t)offset, take);
    *out_read = take;
    return H2_PAL_OK;
}

static h2_pal_result_t inspect(
    test_buffer_t *package,
    uint64_t reported_bytes,
    h2_h2loader_host_catalog_entry_t *out_asset) {
    h2_h2loader_host_package_inspect_config_t config = {
        .allocator = &test_mem,
        .read_payload = read_buffer,
        .payload_user = package,
        .payload_bytes = reported_bytes,
    };
    return h2_h2loader_host_package_inspect(&config, out_asset);
}

static void test_valid_app_package(void) {
    package_options_t options = {
        .first_data_path = "data/version.txt",
    };
    test_buffer_t package = build_package(&options);
    h2_h2loader_host_catalog_entry_t asset;
    char archive_sha[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];

    sha_hex(package.bytes, package.len, archive_sha);
    assert(inspect(&package, package.len, &asset) == H2_PAL_OK);
    assert(asset.role == H2_H2LOADER_HOST_ASSET_ROLE_APP);
    assert(asset.operation ==
        H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL);
    assert(asset.identity_source ==
        H2_H2LOADER_HOST_ASSET_IDENTITY_PACKAGE_MANIFEST);
    assert(strcmp(asset.board, "devkit") == 0);
    assert(strcmp(asset.target, "esp32s3") == 0);
    assert(strcmp(asset.version, "v1.2.3") == 0);
    assert(asset.image[0] == '\0');
    assert(asset.bytes == package.len);
    assert(strcmp(asset.sha256, archive_sha) == 0);
    assert(h2_h2loader_host_is_sha256(asset.image_sha256));
}

static void test_valid_loader_package(void) {
    package_options_t options = {.role = "h2loader"};
    test_buffer_t package = build_package(&options);
    h2_h2loader_host_catalog_entry_t asset;

    assert(inspect(&package, package.len, &asset) == H2_PAL_OK);
    assert(asset.role == H2_H2LOADER_HOST_ASSET_ROLE_LOADER);
    assert(h2_h2loader_host_is_sha256(asset.image_sha256));
}

static void test_loader_rejects_data(void) {
    package_options_t options = {
        .role = "h2loader",
        .first_data_path = "data/version.txt",
    };
    test_buffer_t package = build_package(&options);
    h2_h2loader_host_catalog_entry_t asset;

    assert(inspect(&package, package.len, &asset) == H2_PAL_ERR_FORMAT);
}

static void test_rejects_bad_checksums(void) {
    static const char bad_sha[] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    package_options_t data_options = {
        .first_data_path = "data/version.txt",
        .checksum_override = bad_sha,
    };
    package_options_t image_options = {.image_sha_override = bad_sha};
    test_buffer_t data_package = build_package(&data_options);
    test_buffer_t image_package = build_package(&image_options);
    h2_h2loader_host_catalog_entry_t asset;

    assert(inspect(&data_package, data_package.len, &asset) ==
        H2_PAL_ERR_FORMAT);
    assert(inspect(&image_package, image_package.len, &asset) ==
        H2_PAL_ERR_FORMAT);
}

static void test_rejects_malformed_manifest_and_size(void) {
    package_options_t role_options = {.role = "application"};
    package_options_t size_options = {.image_size_override = "17"};
    package_options_t version_options = {.version_override = "bad/version"};
    test_buffer_t role_package = build_package(&role_options);
    test_buffer_t size_package = build_package(&size_options);
    test_buffer_t version_package = build_package(&version_options);
    h2_h2loader_host_catalog_entry_t asset;

    assert(inspect(&role_package, role_package.len, &asset) ==
        H2_PAL_ERR_FORMAT);
    assert(inspect(&size_package, size_package.len, &asset) ==
        H2_PAL_ERR_FORMAT);
    assert(inspect(&version_package, version_package.len, &asset) ==
        H2_PAL_ERR_FORMAT);
}

static void test_rejects_unsafe_and_duplicate_paths(void) {
    package_options_t unsafe_options = {
        .first_data_path = "data/../secret",
    };
    package_options_t duplicate_options = {
        .first_data_path = "data/version.txt",
        .second_data_path = "data/version.txt",
    };
    package_options_t empty_data_path_options = {
        .first_data_path = "data/",
    };
    package_options_t empty_app_path_options = {.app_path = "app/"};
    test_buffer_t unsafe_package = build_package(&unsafe_options);
    test_buffer_t duplicate_package = build_package(&duplicate_options);
    test_buffer_t empty_data_path_package =
        build_package(&empty_data_path_options);
    test_buffer_t empty_app_path_package =
        build_package(&empty_app_path_options);
    h2_h2loader_host_catalog_entry_t asset;

    assert(inspect(&unsafe_package, unsafe_package.len, &asset) ==
        H2_PAL_ERR_FORMAT);
    assert(inspect(&duplicate_package, duplicate_package.len, &asset) ==
        H2_PAL_ERR_FORMAT);
    assert(inspect(
               &empty_data_path_package,
               empty_data_path_package.len,
               &asset) == H2_PAL_ERR_FORMAT);
    assert(inspect(
               &empty_app_path_package,
               empty_app_path_package.len,
               &asset) == H2_PAL_ERR_FORMAT);
}

static void test_rejects_truncation_and_trailing_bytes(void) {
    package_options_t options = {0};
    test_buffer_t package = build_package(&options);
    h2_h2loader_host_catalog_entry_t asset;
    size_t original_len = package.len;

    assert(inspect(&package, package.len - 1u, &asset) != H2_PAL_OK);
    assert(package.len + 1u <= sizeof(package.bytes));
    package.bytes[package.len++] = 0u;
    assert(inspect(&package, package.len, &asset) == H2_PAL_ERR_FORMAT);
    package.len = original_len;
}

int main(void) {
    test_valid_app_package();
    test_valid_loader_package();
    test_loader_rejects_data();
    test_rejects_bad_checksums();
    test_rejects_malformed_manifest_and_size();
    test_rejects_unsafe_and_duplicate_paths();
    test_rejects_truncation_and_trailing_bytes();
    return 0;
}
