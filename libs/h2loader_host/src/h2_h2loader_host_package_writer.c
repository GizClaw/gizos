#include "h2_h2loader_host_package.h"

#include "h2_h2loader_host_internal.h"
#include "h2_bundle_segmented.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>

#define TAR_BLOCK_SIZE 512u
#define TAR_RECORD_SIZE 10240u
#define PACKAGE_BUFFER_SIZE 4096u

typedef struct package_stream {
    const h2_h2loader_host_package_writer_config_t *config;
    z_stream zlib;
    uint8_t output[PACKAGE_BUFFER_SIZE];
    uint64_t tar_bytes;
    uint64_t package_bytes;
    int raw_tar;
} package_stream_t;

static voidpf package_zalloc(voidpf opaque, uInt items, uInt size) {
    const h2_pal_mem_api_t *allocator = opaque;
    if (size != 0u && items > (uInt)(SIZE_MAX / size)) {
        return NULL;
    }
    return h2_pal_mem_alloc(allocator, (size_t)items * size);
}

static void package_zfree(voidpf opaque, voidpf address) {
    h2_pal_mem_free((const h2_pal_mem_api_t *)opaque, address);
}

static int safe_version(const char *value) {
    size_t len = 0u;
    if (value == NULL) {
        return 0;
    }
    while (value[len] != '\0') {
        unsigned char character = (unsigned char)value[len];
        if (len == 95u || character < 0x21u || character > 0x7eu) {
            return 0;
        }
        ++len;
    }
    return len != 0u;
}

static int safe_package_path(const char *value, int app) {
    const char *cursor;
    const char *segment;
    size_t len;
    if (value == NULL || value[0] == '\0' || value[0] == '/' ||
        strchr(value, '\\') != NULL || strstr(value, "//") != NULL) {
        return 0;
    }
    len = strlen(value);
    if (len > 255u || value[len - 1u] == '/' ||
        strcmp(value, ".") == 0 || strcmp(value, "..") == 0) {
        return 0;
    }
    if ((app && strncmp(value, "app/", 4u) != 0) ||
        (!app && strncmp(value, "data/", 5u) != 0)) {
        return 0;
    }
    for (cursor = value; *cursor != '\0'; ++cursor) {
        if ((unsigned char)*cursor < 0x20u || (unsigned char)*cursor > 0x7eu) {
            return 0;
        }
    }
    segment = value;
    for (cursor = value; ; ++cursor) {
        if (*cursor != '/' && *cursor != '\0') continue;
        if ((size_t)(cursor - segment) == 1u && segment[0] == '.') return 0;
        if ((size_t)(cursor - segment) == 2u &&
            segment[0] == '.' && segment[1] == '.') return 0;
        if (*cursor == '\0') break;
        segment = cursor + 1u;
    }
    return 1;
}

static h2_pal_result_t stream_flush_output(package_stream_t *stream) {
    size_t produced = sizeof(stream->output) - stream->zlib.avail_out;
    h2_pal_result_t rc;
    if (produced == 0u) {
        return H2_PAL_OK;
    }
    rc = stream->config->write(stream->config->write_user, stream->output, produced);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    stream->package_bytes += produced;
    stream->zlib.next_out = stream->output;
    stream->zlib.avail_out = sizeof(stream->output);
    return H2_PAL_OK;
}

static h2_pal_result_t stream_write(package_stream_t *stream, const uint8_t *data, size_t len) {
    if (stream->raw_tar) {
        if (len > UINT64_MAX - stream->tar_bytes) return H2_PAL_ERR_NO_SPACE;
        h2_pal_result_t rc = stream->config->write(stream->config->write_user, data, len);
        if (rc == H2_PAL_OK) {
            stream->tar_bytes += len;
            stream->package_bytes += len;
        }
        return rc;
    }
    while (len != 0u) {
        uInt take = len > UINT_MAX ? UINT_MAX : (uInt)len;
        stream->zlib.next_in = (Bytef *)data;
        stream->zlib.avail_in = take;
        while (stream->zlib.avail_in != 0u) {
            if (deflate(&stream->zlib, Z_NO_FLUSH) != Z_OK) {
                return H2_PAL_ERR_IO;
            }
            if (stream->zlib.avail_out == 0u) {
                h2_pal_result_t rc = stream_flush_output(stream);
                if (rc != H2_PAL_OK) {
                    return rc;
                }
            }
        }
        data += take;
        len -= take;
        stream->tar_bytes += take;
    }
    return H2_PAL_OK;
}

static void tar_octal(char *field, size_t size, uint64_t value) {
    (void)snprintf(field, size, "%0*llo", (int)size - 1, (unsigned long long)value);
}

static int tar_name(uint8_t header[TAR_BLOCK_SIZE], const char *name) {
    size_t len = strlen(name);
    const char *slash;
    if (len <= 100u) {
        memcpy(header, name, len);
        return 1;
    }
    slash = name + len;
    while (slash != name) {
        --slash;
        if (*slash == '/' && (size_t)(slash - name) <= 155u && strlen(slash + 1u) <= 100u) {
            memcpy(&header[345], name, (size_t)(slash - name));
            memcpy(header, slash + 1u, strlen(slash + 1u));
            return 1;
        }
    }
    return 0;
}

static h2_pal_result_t tar_header(package_stream_t *stream, const char *name, uint64_t size) {
    uint8_t header[TAR_BLOCK_SIZE] = {0};
    unsigned int checksum = 0u;
    if (!tar_name(header, name) || size > UINT64_C(077777777777)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    tar_octal((char *)&header[100], 8u, 0644u);
    tar_octal((char *)&header[108], 8u, 0u);
    tar_octal((char *)&header[116], 8u, 0u);
    tar_octal((char *)&header[124], 12u, size);
    tar_octal((char *)&header[136], 12u, 0u);
    memset(&header[148], ' ', 8u);
    header[156] = '0';
    memcpy(&header[257], "ustar\0", 6u);
    memcpy(&header[263], "00", 2u);
    for (size_t i = 0u; i < sizeof(header); ++i) {
        checksum += header[i];
    }
    (void)snprintf((char *)&header[148], 8u, "%06o", checksum);
    header[154] = '\0';
    header[155] = ' ';
    return stream_write(stream, header, sizeof(header));
}

static h2_pal_result_t tar_padding(package_stream_t *stream, uint64_t size) {
    static const uint8_t zeros[TAR_BLOCK_SIZE] = {0};
    size_t padding = (size_t)((TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE);
    return padding == 0u ? H2_PAL_OK : stream_write(stream, zeros, padding);
}

static h2_pal_result_t hash_source(
    const h2_h2loader_host_package_source_t *source,
    h2_h2loader_host_sha256_t *sha) {
    uint8_t buffer[PACKAGE_BUFFER_SIZE];
    uint64_t offset = 0u;
    while (offset != source->size) {
        size_t take = source->size - offset > sizeof(buffer)
            ? sizeof(buffer) : (size_t)(source->size - offset);
        size_t read = 0u;
        h2_pal_result_t rc = source->read(source->user, offset, buffer, take, &read);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (read == 0u || read > take) {
            return H2_PAL_ERR_IO;
        }
        h2_h2loader_host_sha256_update(sha, buffer, read);
        offset += read;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t tar_source(
    package_stream_t *stream,
    const h2_h2loader_host_package_source_t *source) {
    uint8_t buffer[PACKAGE_BUFFER_SIZE];
    uint64_t offset = 0u;
    h2_pal_result_t rc = tar_header(stream, source->name, source->size);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    while (offset != source->size) {
        size_t take = source->size - offset > sizeof(buffer)
            ? sizeof(buffer) : (size_t)(source->size - offset);
        size_t read = 0u;
        rc = source->read(source->user, offset, buffer, take, &read);
        if (rc != H2_PAL_OK || read == 0u || read > take) {
            return rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc;
        }
        rc = stream_write(stream, buffer, read);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        offset += read;
    }
    return tar_padding(stream, source->size);
}

static int source_before(
    const h2_h2loader_host_package_source_t *left,
    const h2_h2loader_host_package_source_t *right) {
    return strcmp(left->name, right->name) < 0;
}

static h2_pal_result_t validate_config(const h2_h2loader_host_package_writer_config_t *config) {
    if (config == NULL || config->allocator == NULL || config->write == NULL ||
        config->app.read == NULL || config->app.size == 0u ||
        !safe_package_path(config->app.name, 1) ||
        (strcmp(config->role == NULL ? "" : config->role, "app") != 0 &&
         strcmp(config->role == NULL ? "" : config->role, "h2loader") != 0) ||
        !h2_h2loader_host_is_safe_identity(config->board) ||
        !h2_h2loader_host_is_safe_identity(config->target) || !safe_version(config->version) ||
        (config->data_entry_count != 0u && config->data_entries == NULL) ||
        config->data_entry_count > SIZE_MAX / sizeof(void *) ||
        (strcmp(config->role, "h2loader") == 0 && config->data_entry_count != 0u) ||
        config->package_format > 2u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < config->data_entry_count; ++i) {
        const h2_h2loader_host_package_source_t *source = &config->data_entries[i];
        if (!safe_package_path(source->name, 0) || source->read == NULL) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        for (size_t j = 0u; j < i; ++j) {
            if (strcmp(source->name, config->data_entries[j].name) == 0) {
                return H2_PAL_ERR_INVALID_ARG;
            }
        }
    }
    return H2_PAL_OK;
}

typedef struct compressed_output {
    package_stream_t *outer;
    h2_h2loader_host_sha256_t sha;
    uint64_t bytes;
} compressed_output_t;

static int compressed_write(void *user, const uint8_t *data, size_t len) {
    compressed_output_t *output = user;
    if (len > UINT64_MAX - output->bytes) return H2_PAL_ERR_NO_SPACE;
    h2_h2loader_host_sha256_update(&output->sha, data, len);
    output->bytes += len;
    return output->outer == NULL ? H2_PAL_OK : stream_write(output->outer, data, len);
}

static int tar_end(package_stream_t *stream) {
    static const uint8_t zeros[TAR_RECORD_SIZE] = {0};
    int rc = stream_write(stream, zeros, TAR_BLOCK_SIZE * 2u);
    size_t padding = (size_t)((TAR_RECORD_SIZE - stream->tar_bytes % TAR_RECORD_SIZE) % TAR_RECORD_SIZE);
    return rc == H2_PAL_OK && padding != 0u ? stream_write(stream, zeros, padding) : rc;
}

static int compress_member(const h2_h2loader_host_package_writer_config_t *config,
    const h2_h2loader_host_package_source_t *const *sorted, int app,
    package_stream_t *outer, h2_bundle_segment_t *out_segment) {
    compressed_output_t output = {.outer = outer};
    h2_h2loader_host_package_writer_config_t sink = *config;
    package_stream_t stream = {.config = &sink};
    uint8_t hash[32];
    int rc = H2_PAL_OK;
    h2_h2loader_host_sha256_init(&output.sha);
    sink.write = compressed_write;
    sink.write_user = &output;
    stream.zlib.zalloc = package_zalloc;
    stream.zlib.zfree = package_zfree;
    stream.zlib.opaque = (voidpf)config->allocator;
    stream.zlib.next_out = stream.output;
    stream.zlib.avail_out = sizeof(stream.output);
    if (deflateInit(&stream.zlib, 6) != Z_OK) return H2_PAL_ERR_NO_MEMORY;
    if (app) {
        uint8_t data[PACKAGE_BUFFER_SIZE];
        uint64_t offset = 0u;
        while (rc == H2_PAL_OK && offset < config->app.size) {
            size_t n = config->app.size - offset > sizeof(data)
                ? sizeof(data) : (size_t)(config->app.size - offset);
            size_t got = 0u;
            rc = config->app.read(config->app.user, offset, data, n, &got);
            if (rc == H2_PAL_OK && (got == 0u || got > n)) rc = H2_PAL_ERR_IO;
            if (rc == H2_PAL_OK) rc = stream_write(&stream, data, got);
            offset += got;
        }
    } else {
        for (size_t i = 0u; rc == H2_PAL_OK && i < config->data_entry_count; ++i)
            rc = tar_source(&stream, sorted[i]);
        if (rc == H2_PAL_OK) rc = tar_end(&stream);
    }
    int zrc = Z_OK;
    while (rc == H2_PAL_OK && zrc != Z_STREAM_END) {
        zrc = deflate(&stream.zlib, Z_FINISH);
        if (zrc != Z_OK && zrc != Z_STREAM_END) { rc = H2_PAL_ERR_IO; break; }
        rc = stream_flush_output(&stream);
    }
    (void)deflateEnd(&stream.zlib);
    if (rc != H2_PAL_OK) return rc;
    out_segment->size = stream.tar_bytes;
    out_segment->compressed_size = output.bytes;
    h2_h2loader_host_sha256_finish(&output.sha, hash);
    h2_h2loader_host_sha256_hex(hash, out_segment->compressed_sha256);
    return H2_PAL_OK;
}

static int write_segmented(const h2_h2loader_host_package_writer_config_t *config,
    const h2_h2loader_host_package_source_t *const *sorted,
    const char *image_hex, const char *data_hex,
    h2_h2loader_host_package_writer_result_t *result) {
    h2_bundle_segment_t app = {0}, data = {0}, written = {0};
    package_stream_t outer = {.config = config, .raw_tar = 1};
    char manifest[H2_BUNDLE_SEGMENTED_MANIFEST_MAX];
    uint64_t data_bytes = 0u, pixa_bytes = 0u;
    for (size_t i = 0u; i < config->data_entry_count; ++i) {
        size_t len = strlen(sorted[i]->name);
        uint64_t *total = len >= 5u && strcmp(sorted[i]->name + len - 5u, ".pixa") == 0
            ? &pixa_bytes : &data_bytes;
        if (sorted[i]->size > UINT64_MAX - *total) return H2_PAL_ERR_NO_SPACE;
        *total += sorted[i]->size;
    }
    int rc = compress_member(config, sorted, 0, NULL, &data);
    if (rc == H2_PAL_OK) rc = compress_member(config, sorted, 1, NULL, &app);
    if (rc != H2_PAL_OK) return rc;
    int n = snprintf(manifest, sizeof(manifest),
        "format=2\nrole=%s\nboard=%s\ntarget=%s\nversion=%s\nimage_size=%llu\n"
        "image_sha256=%s\ndata_sha256=%s\ndata_tar_size=%llu\ndata_bytes=%llu\npixa_bytes=%llu\n"
        "app_zlib_size=%llu\napp_zlib_sha256=%s\ndata_zlib_size=%llu\ndata_zlib_sha256=%s\n",
        config->role, config->board, config->target, config->version,
        (unsigned long long)app.size, image_hex, data_hex, (unsigned long long)data.size,
        (unsigned long long)data_bytes, (unsigned long long)pixa_bytes,
        (unsigned long long)app.compressed_size, app.compressed_sha256,
        (unsigned long long)data.compressed_size, data.compressed_sha256);
    if (n < 0 || (size_t)n >= sizeof(manifest)) return H2_PAL_ERR_NO_SPACE;
    rc = tar_header(&outer, "manifest", (uint64_t)n);
    if (rc == H2_PAL_OK) rc = stream_write(&outer, (const uint8_t *)manifest, (size_t)n);
    if (rc == H2_PAL_OK) rc = tar_padding(&outer, (uint64_t)n);
    const h2_bundle_segment_t *segments[] = {&data, &app};
    const char *names[] = {"data.tar.zlib", "app.bin.zlib"};
    for (size_t i = 0u; rc == H2_PAL_OK && i < 2u; ++i) {
        rc = tar_header(&outer, names[i], segments[i]->compressed_size);
        if (rc == H2_PAL_OK) rc = compress_member(config, sorted, i == 1u, &outer, &written);
        if (rc == H2_PAL_OK && (written.compressed_size != segments[i]->compressed_size ||
            written.size != segments[i]->size ||
            strcmp(written.compressed_sha256, segments[i]->compressed_sha256) != 0))
            rc = H2_PAL_ERR_INVALID_STATE;
        if (rc == H2_PAL_OK) rc = tar_padding(&outer, segments[i]->compressed_size);
    }
    if (rc == H2_PAL_OK) rc = tar_end(&outer);
    if (rc == H2_PAL_OK) {
        result->package_bytes = outer.package_bytes;
        memcpy(result->image_sha256, image_hex, 65u);
        memcpy(result->data_sha256, data_hex, 65u);
    }
    return rc;
}

h2_pal_result_t h2_h2loader_host_package_write(
    const h2_h2loader_host_package_writer_config_t *config,
    h2_h2loader_host_package_writer_result_t *out_result) {
    const h2_h2loader_host_package_source_t **sorted = NULL;
    h2_h2loader_host_sha256_t image_sha;
    h2_h2loader_host_sha256_t data_sha;
    uint8_t digest[32];
    char image_hex[65];
    char data_hex[65];
    char manifest[512];
    char checksum[66];
    h2_h2loader_host_package_source_t generated;
    package_stream_t stream;
    h2_pal_result_t rc = validate_config(config);

    if (rc != H2_PAL_OK || out_result == NULL) {
        return rc != H2_PAL_OK ? rc : H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(*out_result));
    if (config->data_entry_count != 0u) {
        sorted = h2_pal_mem_alloc(config->allocator,
            config->data_entry_count * sizeof(*sorted));
        if (sorted == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        for (size_t i = 0u; i < config->data_entry_count; ++i) {
            sorted[i] = &config->data_entries[i];
            for (size_t j = i; j != 0u && source_before(sorted[j], sorted[j - 1u]); --j) {
                const h2_h2loader_host_package_source_t *swap = sorted[j - 1u];
                sorted[j - 1u] = sorted[j];
                sorted[j] = swap;
            }
        }
    }
    h2_h2loader_host_sha256_init(&image_sha);
    rc = hash_source(&config->app, &image_sha);
    if (rc != H2_PAL_OK) {
        goto cleanup;
    }
    h2_h2loader_host_sha256_finish(&image_sha, digest);
    h2_h2loader_host_sha256_hex(digest, image_hex);
    h2_h2loader_host_sha256_init(&data_sha);
    for (size_t i = 0u; i < config->data_entry_count; ++i) {
        const h2_h2loader_host_package_source_t *source = sorted[i];
        const uint8_t zero = 0u;
        h2_h2loader_host_sha256_update(&data_sha, (const uint8_t *)source->name, strlen(source->name));
        h2_h2loader_host_sha256_update(&data_sha, &zero, 1u);
        rc = hash_source(source, &data_sha);
        if (rc != H2_PAL_OK) {
            goto cleanup;
        }
        h2_h2loader_host_sha256_update(&data_sha, &zero, 1u);
    }
    h2_h2loader_host_sha256_finish(&data_sha, digest);
    h2_h2loader_host_sha256_hex(digest, data_hex);
    if (config->package_format == 2u) {
        rc = write_segmented(config, sorted, image_hex, data_hex, out_result);
        goto cleanup;
    }
    if (snprintf(manifest, sizeof(manifest),
            "format=1\nrole=%s\nboard=%s\ntarget=%s\nversion=%s\nimage_size=%llu\nimage_sha256=%s\n",
            config->role, config->board, config->target, config->version,
            (unsigned long long)config->app.size, image_hex) >= (int)sizeof(manifest)) {
        rc = H2_PAL_ERR_INVALID_ARG;
        goto cleanup;
    }
    (void)snprintf(checksum, sizeof(checksum), "%s\n", data_hex);
    memset(&stream, 0, sizeof(stream));
    stream.config = config;
    stream.zlib.zalloc = package_zalloc;
    stream.zlib.zfree = package_zfree;
    stream.zlib.opaque = (voidpf)config->allocator;
    stream.zlib.next_out = stream.output;
    stream.zlib.avail_out = sizeof(stream.output);
    if (deflateInit(&stream.zlib, 6) != Z_OK) {
        rc = H2_PAL_ERR_NO_MEMORY;
        goto cleanup;
    }
    generated = (h2_h2loader_host_package_source_t){
        .name = "manifest", .size = strlen(manifest),
    };
    rc = tar_header(&stream, generated.name, generated.size);
    if (rc == H2_PAL_OK) rc = stream_write(&stream, (const uint8_t *)manifest, generated.size);
    if (rc == H2_PAL_OK) rc = tar_padding(&stream, generated.size);
    generated.name = "checksum";
    generated.size = strlen(checksum);
    if (rc == H2_PAL_OK) rc = tar_header(&stream, generated.name, generated.size);
    if (rc == H2_PAL_OK) rc = stream_write(&stream, (const uint8_t *)checksum, generated.size);
    if (rc == H2_PAL_OK) rc = tar_padding(&stream, generated.size);
    for (size_t i = 0u; rc == H2_PAL_OK && i < config->data_entry_count; ++i) {
        rc = tar_source(&stream, sorted[i]);
    }
    if (rc == H2_PAL_OK) rc = tar_source(&stream, &config->app);
    if (rc == H2_PAL_OK) {
        static const uint8_t zeros[TAR_RECORD_SIZE] = {0};
        size_t padding;
        rc = stream_write(&stream, zeros, TAR_BLOCK_SIZE * 2u);
        padding = (size_t)((TAR_RECORD_SIZE - (stream.tar_bytes % TAR_RECORD_SIZE)) % TAR_RECORD_SIZE);
        if (rc == H2_PAL_OK && padding != 0u) rc = stream_write(&stream, zeros, padding);
    }
    if (rc == H2_PAL_OK) {
        int zrc;
        do {
            zrc = deflate(&stream.zlib, Z_FINISH);
            if (zrc != Z_OK && zrc != Z_STREAM_END) {
                rc = H2_PAL_ERR_IO;
                break;
            }
            rc = stream_flush_output(&stream);
        } while (rc == H2_PAL_OK && zrc != Z_STREAM_END);
    }
    (void)deflateEnd(&stream.zlib);
    if (rc == H2_PAL_OK) {
        out_result->package_bytes = stream.package_bytes;
        memcpy(out_result->image_sha256, image_hex, sizeof(image_hex));
        memcpy(out_result->data_sha256, data_hex, sizeof(data_hex));
    }
cleanup:
    h2_pal_mem_free(config->allocator, sorted);
    return rc;
}
