#include "h2_loader_metadata.h"

#include <string.h>

#define H2_LOADER_METADATA_MAGIC UINT32_C(0x484d4432)
#define H2_LOADER_METADATA_FORMAT UINT32_C(1)
#define H2_LOADER_METADATA_HEADER_SIZE 36u
#define H2_LOADER_METADATA_ENCODED_SIZE \
    (H2_LOADER_METADATA_HEADER_SIZE + (H2_LOADER_SHA256_HEX_SIZE * 2u) + \
     (H2_LOADER_IDENTITY_TEXT_MAX * 3u))

static int is_hex(char value) {
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

static int checksum_valid(const char *value, int optional) {
    size_t i;
    if (value == NULL) return 0;
    if (value[0] == '\0') return optional;
    for (i = 0u; i < H2_LOADER_SHA256_HEX_SIZE - 1u; ++i) {
        if (!is_hex(value[i])) return 0;
    }
    return value[H2_LOADER_SHA256_HEX_SIZE - 1u] == '\0';
}

static int text_valid(const char *value) {
    size_t len = 0u;
    if (value == NULL || value[0] == '\0') return 0;
    while (len < H2_LOADER_IDENTITY_TEXT_MAX && value[len] != '\0') {
        ++len;
    }
    return len > 0u && len < H2_LOADER_IDENTITY_TEXT_MAX;
}

static void put_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value & UINT32_C(0xff));
    data[1] = (uint8_t)((value >> 8u) & UINT32_C(0xff));
    data[2] = (uint8_t)((value >> 16u) & UINT32_C(0xff));
    data[3] = (uint8_t)((value >> 24u) & UINT32_C(0xff));
}

static uint32_t get_u32(const uint8_t *data) {
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8u) |
        ((uint32_t)data[2] << 16u) |
        ((uint32_t)data[3] << 24u);
}

static void put_u64(uint8_t *data, uint64_t value) {
    size_t i;
    for (i = 0u; i < 8u; ++i) {
        data[i] = (uint8_t)((value >> (i * 8u)) & UINT64_C(0xff));
    }
}

static uint64_t get_u64(const uint8_t *data) {
    uint64_t value = 0u;
    size_t i;
    for (i = 0u; i < 8u; ++i) {
        value |= (uint64_t)data[i] << (i * 8u);
    }
    return value;
}

const char *h2_loader_metadata_slot_key(h2_loader_metadata_slot_t slot) {
    switch (slot) {
        case H2_LOADER_METADATA_SLOT_STAGE:
            return "stage";
        case H2_LOADER_METADATA_SLOT_PARTITION_1:
            return "partition_1";
        case H2_LOADER_METADATA_SLOT_PARTITION_2:
            return "partition_2";
        default:
            return NULL;
    }
}

int h2_loader_metadata_validate(
    h2_loader_metadata_slot_t slot,
    const h2_loader_metadata_t *metadata) {
    if (h2_loader_metadata_slot_key(slot) == NULL || metadata == NULL ||
        (metadata->valid != 0 && metadata->valid != 1)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!metadata->valid) return H2_PAL_OK;
    if (!checksum_valid(metadata->image_checksum, 0) ||
        metadata->image_size == 0u ||
        (metadata->role != H2_LOADER_IMAGE_ROLE_APP &&
         metadata->role != H2_LOADER_IMAGE_ROLE_H2LOADER) ||
        !text_valid(metadata->version) || !text_valid(metadata->board) ||
        !text_valid(metadata->target)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (slot == H2_LOADER_METADATA_SLOT_STAGE) {
        if (!checksum_valid(metadata->package_checksum, 0) ||
            metadata->package_size == 0u) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    } else if (!checksum_valid(metadata->package_checksum, 1) ||
               (metadata->package_checksum[0] != '\0' &&
                metadata->package_size == 0u) ||
               (metadata->package_checksum[0] == '\0' &&
                metadata->package_size != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

int h2_loader_metadata_encode(
    h2_loader_metadata_slot_t slot,
    const h2_loader_metadata_t *metadata,
    void *data,
    size_t capacity,
    size_t *out_len) {
    uint8_t *bytes = (uint8_t *)data;
    size_t offset = H2_LOADER_METADATA_HEADER_SIZE;
    int rc;
    if (data == NULL || out_len == NULL ||
        capacity < H2_LOADER_METADATA_ENCODED_SIZE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_loader_metadata_validate(slot, metadata);
    if (rc != H2_PAL_OK) return rc;
    memset(bytes, 0, H2_LOADER_METADATA_ENCODED_SIZE);
    put_u32(bytes, H2_LOADER_METADATA_MAGIC);
    put_u32(bytes + 4u, H2_LOADER_METADATA_FORMAT);
    put_u32(bytes + 8u, (uint32_t)slot);
    put_u32(bytes + 12u, metadata->valid ? UINT32_C(1) : UINT32_C(0));
    put_u32(bytes + 16u, (uint32_t)metadata->role);
    put_u64(bytes + 20u, metadata->package_size);
    put_u64(bytes + 28u, metadata->image_size);
    memcpy(bytes + offset, metadata->package_checksum, H2_LOADER_SHA256_HEX_SIZE);
    offset += H2_LOADER_SHA256_HEX_SIZE;
    memcpy(bytes + offset, metadata->image_checksum, H2_LOADER_SHA256_HEX_SIZE);
    offset += H2_LOADER_SHA256_HEX_SIZE;
    memcpy(bytes + offset, metadata->version, H2_LOADER_IDENTITY_TEXT_MAX);
    offset += H2_LOADER_IDENTITY_TEXT_MAX;
    memcpy(bytes + offset, metadata->board, H2_LOADER_IDENTITY_TEXT_MAX);
    offset += H2_LOADER_IDENTITY_TEXT_MAX;
    memcpy(bytes + offset, metadata->target, H2_LOADER_IDENTITY_TEXT_MAX);
    *out_len = H2_LOADER_METADATA_ENCODED_SIZE;
    return H2_PAL_OK;
}

int h2_loader_metadata_decode(
    h2_loader_metadata_slot_t slot,
    const void *data,
    size_t len,
    h2_loader_metadata_t *out_metadata) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = H2_LOADER_METADATA_HEADER_SIZE;
    if (data == NULL || out_metadata == NULL ||
        len != H2_LOADER_METADATA_ENCODED_SIZE ||
        get_u32(bytes) != H2_LOADER_METADATA_MAGIC ||
        get_u32(bytes + 4u) != H2_LOADER_METADATA_FORMAT ||
        get_u32(bytes + 8u) != (uint32_t)slot ||
        get_u32(bytes + 12u) > UINT32_C(1)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_metadata, 0, sizeof(*out_metadata));
    out_metadata->valid = (int)get_u32(bytes + 12u);
    out_metadata->role = (h2_loader_image_role_t)get_u32(bytes + 16u);
    out_metadata->package_size = get_u64(bytes + 20u);
    out_metadata->image_size = get_u64(bytes + 28u);
    memcpy(out_metadata->package_checksum, bytes + offset, H2_LOADER_SHA256_HEX_SIZE);
    offset += H2_LOADER_SHA256_HEX_SIZE;
    memcpy(out_metadata->image_checksum, bytes + offset, H2_LOADER_SHA256_HEX_SIZE);
    offset += H2_LOADER_SHA256_HEX_SIZE;
    memcpy(out_metadata->version, bytes + offset, H2_LOADER_IDENTITY_TEXT_MAX);
    offset += H2_LOADER_IDENTITY_TEXT_MAX;
    memcpy(out_metadata->board, bytes + offset, H2_LOADER_IDENTITY_TEXT_MAX);
    offset += H2_LOADER_IDENTITY_TEXT_MAX;
    memcpy(out_metadata->target, bytes + offset, H2_LOADER_IDENTITY_TEXT_MAX);
    if (out_metadata->package_checksum[H2_LOADER_SHA256_HEX_SIZE - 1u] != '\0' ||
        out_metadata->image_checksum[H2_LOADER_SHA256_HEX_SIZE - 1u] != '\0' ||
        out_metadata->version[H2_LOADER_IDENTITY_TEXT_MAX - 1u] != '\0' ||
        out_metadata->board[H2_LOADER_IDENTITY_TEXT_MAX - 1u] != '\0' ||
        out_metadata->target[H2_LOADER_IDENTITY_TEXT_MAX - 1u] != '\0') {
        memset(out_metadata, 0, sizeof(*out_metadata));
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_loader_metadata_validate(slot, out_metadata);
}

int h2_loader_metadata_read(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    h2_loader_metadata_slot_t slot,
    h2_loader_metadata_t *out_metadata,
    int *out_present) {
    h2_pal_pref_namespace_t *ns = NULL;
    void *data = NULL;
    size_t len = 0u;
    const char *key = h2_loader_metadata_slot_key(slot);
    int rc;
    int close_rc;
    if (pref == NULL || allocator == NULL || out_metadata == NULL ||
        out_present == NULL || key == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_metadata, 0, sizeof(*out_metadata));
    *out_present = 0;
    rc = h2_pal_pref_open(
        pref, H2_LOADER_PREF_NAMESPACE, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
    if (rc != H2_PAL_OK) return rc;
    if (ns == NULL || ns->get_blob == NULL || ns->close == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->get_blob(ns, allocator, key, &data, &len);
        if (rc == H2_PAL_ERR_NOT_FOUND) {
            rc = H2_PAL_OK;
        } else if (rc == H2_PAL_OK) {
            rc = h2_loader_metadata_decode(slot, data, len, out_metadata);
            if (rc == H2_PAL_OK) *out_present = 1;
        }
    }
    if (data != NULL) h2_pal_mem_free(allocator, data);
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

int h2_loader_metadata_write(
    const h2_pal_pref_api_t *pref,
    h2_loader_metadata_slot_t slot,
    const h2_loader_metadata_t *metadata) {
    uint8_t data[H2_LOADER_METADATA_ENCODED_SIZE];
    size_t len = 0u;
    h2_pal_pref_namespace_t *ns = NULL;
    const char *key = h2_loader_metadata_slot_key(slot);
    int rc;
    int close_rc;
    if (pref == NULL || key == NULL) return H2_PAL_ERR_INVALID_ARG;
    rc = h2_loader_metadata_encode(slot, metadata, data, sizeof(data), &len);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_pal_pref_open(
        pref, H2_LOADER_PREF_NAMESPACE, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc != H2_PAL_OK) return rc;
    if (ns == NULL || ns->set_blob == NULL || ns->commit == NULL ||
        ns->close == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_blob(ns, key, data, len);
        if (rc == H2_PAL_OK) rc = ns->commit(ns);
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

int h2_loader_metadata_clear(
    const h2_pal_pref_api_t *pref,
    h2_loader_metadata_slot_t slot) {
    h2_pal_pref_namespace_t *ns = NULL;
    const char *key = h2_loader_metadata_slot_key(slot);
    int rc;
    int close_rc;
    if (pref == NULL || key == NULL) return H2_PAL_ERR_INVALID_ARG;
    rc = h2_pal_pref_open(
        pref, H2_LOADER_PREF_NAMESPACE, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc != H2_PAL_OK) return rc;
    if (ns == NULL || ns->remove == NULL || ns->commit == NULL ||
        ns->close == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->remove(ns, key);
        if (rc == H2_PAL_ERR_NOT_FOUND) rc = H2_PAL_OK;
        if (rc == H2_PAL_OK) rc = ns->commit(ns);
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

int h2_loader_metadata_from_stage(
    const h2_loader_metadata_t *stage,
    h2_loader_metadata_t *out_partition) {
    if (stage == NULL || out_partition == NULL ||
        h2_loader_metadata_validate(H2_LOADER_METADATA_SLOT_STAGE, stage) !=
            H2_PAL_OK ||
        !stage->valid) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_partition = *stage;
    return H2_PAL_OK;
}

int h2_loader_metadata_image_equal(
    const h2_loader_metadata_t *a,
    const h2_loader_metadata_t *b) {
    if (a == NULL || b == NULL || !a->valid || !b->valid) return 0;
    return a->image_size == b->image_size && a->role == b->role &&
        strcmp(a->image_checksum, b->image_checksum) == 0 &&
        strcmp(a->version, b->version) == 0 &&
        strcmp(a->board, b->board) == 0 &&
        strcmp(a->target, b->target) == 0;
}
