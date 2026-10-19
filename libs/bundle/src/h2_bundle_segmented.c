#include "h2_bundle_segmented.h"

#include "h2_bundle_tar.h"

#include <string.h>
#include <zlib.h>

#define SEGMENT_IO_SIZE 4096u

int h2_bundle_segmented_file_read(
    void *user, uint64_t offset, uint8_t *data, size_t capacity, size_t *out_read) {
    h2_bundle_segmented_file_t *source = user;
    if (out_read != NULL) *out_read = 0u;
    if (source == NULL || source->fs == NULL || source->file == NULL ||
        data == NULL || out_read == NULL || offset > UINT64_MAX - capacity)
        return H2_PAL_ERR_INVALID_ARG;
    if (source->position != offset) {
        int rc = h2_pal_fs_seek(source->fs, source->file, offset);
        if (rc != H2_PAL_OK) return rc;
        source->position = offset;
    }
    int rc = h2_pal_fs_read(source->fs, source->file, data, capacity, out_read);
    if (rc == H2_PAL_OK) {
        if (*out_read > capacity) return H2_PAL_ERR_IO;
        source->position += *out_read;
    }
    return rc;
}

int h2_bundle_digest_valid(const h2_bundle_digest_api_t *digest) {
    return digest != NULL && digest->start != NULL && digest->update != NULL &&
        digest->finish != NULL && digest->abort != NULL;
}

void h2_bundle_digest_hex(const uint8_t digest[32], char out_hex[65]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < 32u; ++i) {
        out_hex[2u * i] = hex[digest[i] >> 4u];
        out_hex[2u * i + 1u] = hex[digest[i] & 15u];
    }
    out_hex[64] = '\0';
}

static int line(const uint8_t **cursor, const uint8_t *end, const char *key,
                const uint8_t **value, size_t *len) {
    size_t key_len = strlen(key);
    const uint8_t *next = memchr(*cursor, '\n', (size_t)(end - *cursor));
    if (next == NULL || (size_t)(next - *cursor) <= key_len ||
        memcmp(*cursor, key, key_len) != 0 || (*cursor)[key_len] != '=') {
        return H2_PAL_ERR_FORMAT;
    }
    *value = *cursor + key_len + 1u;
    *len = (size_t)(next - *value);
    *cursor = next + 1u;
    return *len != 0u ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

static int text_value(char *out, const uint8_t *value, size_t len, int token) {
    if (len == 0u || len >= H2_BUNDLE_SEGMENTED_TEXT_SIZE) return H2_PAL_ERR_FORMAT;
    for (size_t i = 0u; i < len; ++i) {
        uint8_t c = value[i];
        if (token ? !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-') : (c < 0x21u || c > 0x7eu)) {
            return H2_PAL_ERR_FORMAT;
        }
    }
    memcpy(out, value, len);
    out[len] = '\0';
    return H2_PAL_OK;
}

static int sha_value(char *out, const uint8_t *value, size_t len) {
    if (len != 64u) return H2_PAL_ERR_FORMAT;
    for (size_t i = 0u; i < len; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return H2_PAL_ERR_FORMAT;
    }
    memcpy(out, value, 64u);
    out[64] = '\0';
    return H2_PAL_OK;
}

static int number_value(uint64_t *out, const uint8_t *value, size_t len) {
    uint64_t n = 0u;
    for (size_t i = 0u; i < len; ++i) {
        if (value[i] < '0' || value[i] > '9' ||
            n > (UINT64_MAX - (value[i] - '0')) / 10u) return H2_PAL_ERR_FORMAT;
        n = n * 10u + (value[i] - '0');
    }
    *out = n;
    return H2_PAL_OK;
}

int h2_bundle_segmented_manifest_parse(
    const uint8_t *data, size_t len, h2_bundle_segmented_manifest_t *out_manifest) {
    h2_bundle_segmented_manifest_t m = {0};
    const uint8_t *cursor = data;
    const uint8_t *end;
    const uint8_t *value;
    size_t value_len;
    int rc;
    if (out_manifest != NULL) memset(out_manifest, 0, sizeof(*out_manifest));
    if (data == NULL || out_manifest == NULL || len == 0u ||
        len > H2_BUNDLE_SEGMENTED_MANIFEST_MAX) return H2_PAL_ERR_INVALID_ARG;
    end = data + len;
#define READ(key) do { rc = line(&cursor, end, key, &value, &value_len); \
    if (rc != H2_PAL_OK) return rc; } while (0)
#define TEXT(key, field, token) do { READ(key); \
    rc = text_value(m.field, value, value_len, token); if (rc != H2_PAL_OK) return rc; } while (0)
#define SHA(key, field) do { READ(key); \
    rc = sha_value(m.field, value, value_len); if (rc != H2_PAL_OK) return rc; } while (0)
#define NUMBER(key, field) do { READ(key); \
    rc = number_value(&m.field, value, value_len); if (rc != H2_PAL_OK) return rc; } while (0)
    READ("format");
    if (value_len != 1u || value[0] != '2') return H2_PAL_ERR_FORMAT;
    TEXT("role", role, 1);
    if (strcmp(m.role, "app") != 0 && strcmp(m.role, "h2loader") != 0)
        return H2_PAL_ERR_FORMAT;
    TEXT("board", board, 1);
    TEXT("target", target, 1);
    TEXT("version", version, 0);
    NUMBER("image_size", app.size);
    SHA("image_sha256", image_sha256);
    SHA("data_sha256", data_sha256);
    NUMBER("data_tar_size", data.size);
    NUMBER("data_bytes", data_bytes);
    NUMBER("pixa_bytes", pixa_bytes);
    NUMBER("app_zlib_size", app.compressed_size);
    SHA("app_zlib_sha256", app.compressed_sha256);
    NUMBER("data_zlib_size", data.compressed_size);
    SHA("data_zlib_sha256", data.compressed_sha256);
#undef NUMBER
#undef SHA
#undef TEXT
#undef READ
    if (cursor != end || m.app.size == 0u || m.data.size < 1024u || m.data.size % 512u != 0u ||
        m.app.compressed_size < 6u || m.data.compressed_size < 6u ||
        m.data_bytes > m.data.size || m.pixa_bytes > m.data.size - m.data_bytes)
        return H2_PAL_ERR_FORMAT;
    if (strcmp(m.role, "h2loader") == 0 &&
        strcmp(m.data_sha256,
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") != 0)
        return H2_PAL_ERR_FORMAT;
    *out_manifest = m;
    return H2_PAL_OK;
}

static int read_exact(h2_bundle_segmented_read_fn read, void *user,
                      uint64_t offset, uint8_t *data, size_t len) {
    if (offset > UINT64_MAX - len) return H2_PAL_ERR_FORMAT;
    size_t total = 0u;
    while (total < len) {
        size_t n = 0u;
        int rc = read(user, offset + total, data + total, len - total, &n);
        if (rc != H2_PAL_OK) return rc;
        if (n == 0u || n > len - total) return H2_PAL_ERR_TRUNCATED;
        total += n;
    }
    return H2_PAL_OK;
}

static int zero_bytes(h2_bundle_segmented_read_fn read, void *user,
                      uint64_t offset, uint64_t len) {
    uint8_t data[512];
    while (len != 0u) {
        size_t n = len > sizeof(data) ? sizeof(data) : (size_t)len;
        int rc = read_exact(read, user, offset, data, n);
        if (rc != H2_PAL_OK) return rc;
        for (size_t i = 0u; i < n; ++i) {
            if (data[i] != 0u) return H2_PAL_ERR_FORMAT;
        }
        offset += n;
        len -= n;
    }
    return H2_PAL_OK;
}

static int member(h2_bundle_segmented_read_fn read, void *user, uint64_t total,
                  uint64_t offset, const char *name, h2_bundle_entry_t *entry) {
    uint8_t header[512];
    int kind = 0;
    if (offset > total || total - offset < sizeof(header)) return H2_PAL_ERR_TRUNCATED;
    int rc = read_exact(read, user, offset, header, sizeof(header));
    if (rc != H2_PAL_OK) return rc;
    rc = h2_bundle_tar_parse_header(header, entry, &kind);
    if (rc != H2_BUNDLE_OK || kind != H2_BUNDLE_TAR_HEADER_ENTRY ||
        entry->kind != H2_BUNDLE_ENTRY_FILE || strcmp(entry->path, name) != 0 ||
        entry->size == 0u || entry->size > total - offset - sizeof(header) ||
        h2_bundle_tar_padding(entry->size) > total - offset - sizeof(header) - entry->size)
        return H2_PAL_ERR_FORMAT;
    return H2_PAL_OK;
}

static int verify_member(h2_bundle_segmented_read_fn read, void *user,
                         const h2_bundle_segment_t *segment,
                         const h2_bundle_digest_api_t *digest) {
    uint8_t data[SEGMENT_IO_SIZE], hash[32];
    char hex[65];
    uint64_t offset = 0u;
    int rc = digest->start(digest->user);
    while (rc == H2_PAL_OK && offset < segment->compressed_size) {
        size_t n = segment->compressed_size - offset > sizeof(data)
            ? sizeof(data) : (size_t)(segment->compressed_size - offset);
        rc = read_exact(read, user, segment->offset + offset, data, n);
        if (rc == H2_PAL_OK) rc = digest->update(digest->user, data, n);
        offset += n;
    }
    if (rc == H2_PAL_OK) rc = digest->finish(digest->user, hash);
    digest->abort(digest->user);
    if (rc != H2_PAL_OK) return rc;
    h2_bundle_digest_hex(hash, hex);
    return strcmp(hex, segment->compressed_sha256) == 0 ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

int h2_bundle_segmented_inspect(
    h2_bundle_segmented_read_fn read, void *read_user, uint64_t package_size,
    const h2_bundle_digest_api_t *digest, h2_bundle_segmented_manifest_t *out_manifest) {
    h2_bundle_segmented_manifest_t m;
    h2_bundle_entry_t entry;
    uint8_t text[H2_BUNDLE_SEGMENTED_MANIFEST_MAX];
    uint64_t offset = 0u;
    int rc;
    if (out_manifest != NULL) memset(out_manifest, 0, sizeof(*out_manifest));
    if (read == NULL || out_manifest == NULL || !h2_bundle_digest_valid(digest))
        return H2_PAL_ERR_INVALID_ARG;
    if (package_size < 2048u || package_size % 512u != 0u) return H2_PAL_ERR_FORMAT;
    rc = member(read, read_user, package_size, offset, "manifest", &entry);
    if (rc != H2_PAL_OK) return rc;
    if (entry.size > sizeof(text)) return H2_PAL_ERR_FORMAT;
    rc = read_exact(read, read_user, 512u, text, (size_t)entry.size);
    if (rc == H2_PAL_OK) rc = h2_bundle_segmented_manifest_parse(text, (size_t)entry.size, &m);
    if (rc == H2_PAL_OK) rc = zero_bytes(read, read_user, 512u + entry.size,
                                        h2_bundle_tar_padding(entry.size));
    if (rc != H2_PAL_OK) return rc;
    offset = 512u + entry.size + h2_bundle_tar_padding(entry.size);
    h2_bundle_segment_t *segments[] = {&m.data, &m.app};
    const char *names[] = {"data.tar.zlib", "app.bin.zlib"};
    for (size_t i = 0u; i < 2u; ++i) {
        rc = member(read, read_user, package_size, offset, names[i], &entry);
        if (rc != H2_PAL_OK) return rc;
        if (entry.size != segments[i]->compressed_size) return H2_PAL_ERR_FORMAT;
        segments[i]->offset = offset + 512u;
        rc = verify_member(read, read_user, segments[i], digest);
        if (rc == H2_PAL_OK) rc = zero_bytes(read, read_user,
            segments[i]->offset + entry.size, h2_bundle_tar_padding(entry.size));
        if (rc != H2_PAL_OK) return rc;
        offset = segments[i]->offset + entry.size + h2_bundle_tar_padding(entry.size);
    }
    if (package_size - offset < 1024u) return H2_PAL_ERR_TRUNCATED;
    rc = zero_bytes(read, read_user, offset, package_size - offset);
    if (rc == H2_PAL_OK) *out_manifest = m;
    return rc;
}

static voidpf alloc_zlib(voidpf opaque, uInt items, uInt size) {
    if (size != 0u && (size_t)items > SIZE_MAX / size) return NULL;
    return h2_pal_mem_alloc((const h2_pal_mem_api_t *)opaque, (size_t)items * size);
}

static void free_zlib(voidpf opaque, voidpf address) {
    h2_pal_mem_free((const h2_pal_mem_api_t *)opaque, address);
}

int h2_bundle_segmented_inflate(
    h2_bundle_segmented_read_fn read, void *read_user,
    const h2_bundle_segment_t *segment, const h2_pal_mem_api_t *allocator,
    h2_bundle_segmented_write_fn write, void *write_user) {
    uint8_t input[SEGMENT_IO_SIZE], output[SEGMENT_IO_SIZE];
    z_stream stream = {0};
    uint64_t consumed = 0u, produced = 0u;
    int rc = H2_PAL_OK, ended = 0;
    if (read == NULL || segment == NULL || allocator == NULL || write == NULL ||
        segment->compressed_size == 0u || segment->size == 0u ||
        segment->offset > UINT64_MAX - segment->compressed_size)
        return H2_PAL_ERR_INVALID_ARG;
    stream.zalloc = alloc_zlib;
    stream.zfree = free_zlib;
    stream.opaque = (voidpf)allocator;
    int zrc = inflateInit(&stream);
    if (zrc != Z_OK) return zrc == Z_MEM_ERROR ? H2_PAL_ERR_NO_MEMORY : H2_PAL_ERR_FORMAT;
    while (rc == H2_PAL_OK && consumed < segment->compressed_size && !ended) {
        size_t n = segment->compressed_size - consumed > sizeof(input)
            ? sizeof(input) : (size_t)(segment->compressed_size - consumed);
        rc = read_exact(read, read_user, segment->offset + consumed, input, n);
        if (rc != H2_PAL_OK) break;
        consumed += n;
        stream.next_in = input;
        stream.avail_in = (uInt)n;
        do {
            uInt before = stream.avail_in;
            stream.next_out = output;
            stream.avail_out = sizeof(output);
            zrc = inflate(&stream, Z_NO_FLUSH);
            size_t have = sizeof(output) - stream.avail_out;
            /* A full output buffer can require another inflate call after all
             * input was consumed. Fetch the next chunk when that call stalls. */
            if (zrc == Z_BUF_ERROR && before == 0u && have == 0u) break;
            if (zrc != Z_OK && zrc != Z_STREAM_END) {
                rc = zrc == Z_MEM_ERROR ? H2_PAL_ERR_NO_MEMORY : H2_PAL_ERR_FORMAT;
                break;
            }
            if (have > segment->size - produced) { rc = H2_PAL_ERR_FORMAT; break; }
            produced += have;
            if (have != 0u) rc = write(write_user, output, have);
            if (rc != H2_PAL_OK) break;
            if (zrc == Z_STREAM_END) {
                ended = 1;
                if (stream.avail_in != 0u || consumed != segment->compressed_size)
                    rc = H2_PAL_ERR_FORMAT;
                break;
            }
            if (before == stream.avail_in && have == 0u) { rc = H2_PAL_ERR_FORMAT; break; }
        } while (stream.avail_in != 0u || stream.avail_out == 0u);
    }
    (void)inflateEnd(&stream);
    if (rc == H2_PAL_OK && (!ended || produced != segment->size)) rc = H2_PAL_ERR_TRUNCATED;
    return rc;
}
