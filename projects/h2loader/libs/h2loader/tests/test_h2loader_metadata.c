#include "h2_loader_metadata.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define TEST_IMAGE_SHA256 \
    "abababababababababababababababababababababababababababababababab"
#define TEST_PACKAGE_SHA256 TEST_IMAGE_SHA256

typedef struct metadata_pref_fixture {
    h2_pal_pref_namespace_t ns;
    uint8_t persisted[1024];
    uint8_t pending[1024];
    size_t persisted_len;
    size_t pending_len;
    int pending_remove;
    int set_blob_result;
    int remove_result;
    int commit_result;
    unsigned commits;
} metadata_pref_fixture_t;

static void *metadata_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void metadata_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static int metadata_pref_close(h2_pal_pref_namespace_t *ns) {
    (void)ns;
    return H2_PAL_OK;
}

static int metadata_pref_get_blob(
    h2_pal_pref_namespace_t *ns,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    void **out_data,
    size_t *out_len) {
    metadata_pref_fixture_t *fixture = ns->user;
    assert(strcmp(key, "stage") == 0);
    if (fixture->persisted_len == 0u) return H2_PAL_ERR_NOT_FOUND;
    *out_data = h2_pal_mem_alloc(allocator, fixture->persisted_len);
    assert(*out_data != NULL);
    memcpy(*out_data, fixture->persisted, fixture->persisted_len);
    *out_len = fixture->persisted_len;
    return H2_PAL_OK;
}

static int metadata_pref_set_blob(
    h2_pal_pref_namespace_t *ns,
    const char *key,
    const void *data,
    size_t len) {
    metadata_pref_fixture_t *fixture = ns->user;
    assert(strcmp(key, "stage") == 0);
    if (fixture->set_blob_result != H2_PAL_OK)
        return fixture->set_blob_result;
    assert(len <= sizeof(fixture->pending));
    memcpy(fixture->pending, data, len);
    fixture->pending_len = len;
    fixture->pending_remove = 0;
    return H2_PAL_OK;
}

static int metadata_pref_remove(
    h2_pal_pref_namespace_t *ns,
    const char *key) {
    metadata_pref_fixture_t *fixture = ns->user;
    assert(strcmp(key, "stage") == 0);
    if (fixture->remove_result != H2_PAL_OK)
        return fixture->remove_result;
    fixture->pending_remove = 1;
    return fixture->persisted_len == 0u
        ? H2_PAL_ERR_NOT_FOUND : H2_PAL_OK;
}

static int metadata_pref_commit(h2_pal_pref_namespace_t *ns) {
    metadata_pref_fixture_t *fixture = ns->user;
    ++fixture->commits;
    if (fixture->commit_result != H2_PAL_OK) return fixture->commit_result;
    if (fixture->pending_remove) {
        fixture->persisted_len = 0u;
    } else {
        memcpy(fixture->persisted, fixture->pending, fixture->pending_len);
        fixture->persisted_len = fixture->pending_len;
    }
    return H2_PAL_OK;
}

static int metadata_pref_open(
    void *user,
    const char *name_space,
    h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_namespace) {
    metadata_pref_fixture_t *fixture = user;
    (void)mode;
    assert(strcmp(name_space, H2_LOADER_PREF_NAMESPACE) == 0);
    fixture->ns = (h2_pal_pref_namespace_t){
        .user = fixture,
        .close = metadata_pref_close,
        .get_blob = metadata_pref_get_blob,
        .set_blob = metadata_pref_set_blob,
        .remove = metadata_pref_remove,
        .commit = metadata_pref_commit,
    };
    *out_namespace = &fixture->ns;
    return H2_PAL_OK;
}

static h2_loader_metadata_t valid_stage(void) {
    h2_loader_metadata_t metadata = {
        .valid = 1,
        .package_size = UINT64_C(0x100000002),
        .image_size = UINT64_C(0x200000003),
        .role = H2_LOADER_IMAGE_ROLE_H2LOADER,
    };
    (void)snprintf(
        metadata.package_checksum,
        sizeof(metadata.package_checksum),
        "%s",
        TEST_PACKAGE_SHA256);
    (void)snprintf(
        metadata.image_checksum,
        sizeof(metadata.image_checksum),
        "%s",
        TEST_IMAGE_SHA256);
    (void)snprintf(metadata.version, sizeof(metadata.version), "%s", "0.2.0");
    (void)snprintf(metadata.board, sizeof(metadata.board), "%s", "devkit");
    (void)snprintf(metadata.target, sizeof(metadata.target), "%s", "esp32s3");
    return metadata;
}

static void test_slot_keys(void) {
    assert(strcmp(
               h2_loader_metadata_slot_key(H2_LOADER_METADATA_SLOT_STAGE),
               "stage") == 0);
    assert(strcmp(
               h2_loader_metadata_slot_key(H2_LOADER_METADATA_SLOT_PARTITION_1),
               "partition_1") == 0);
    assert(strcmp(
               h2_loader_metadata_slot_key(H2_LOADER_METADATA_SLOT_PARTITION_2),
               "partition_2") == 0);
    assert(h2_loader_metadata_slot_key((h2_loader_metadata_slot_t)0) == NULL);
}

static void test_round_trip_preserves_complete_identity(void) {
    const h2_loader_metadata_t stage = valid_stage();
    h2_loader_metadata_t decoded = {0};
    uint8_t encoded[1024];
    size_t encoded_len = 0u;
    assert(h2_loader_metadata_encode(
               H2_LOADER_METADATA_SLOT_STAGE,
               &stage,
               encoded,
               sizeof(encoded),
               &encoded_len) == H2_PAL_OK);
    assert(encoded_len > 0u);
    assert(h2_loader_metadata_decode(
               H2_LOADER_METADATA_SLOT_STAGE,
               encoded,
               encoded_len,
               &decoded) == H2_PAL_OK);
    assert(decoded.valid == 1);
    assert(decoded.package_size == stage.package_size);
    assert(decoded.image_size == stage.image_size);
    assert(decoded.role == stage.role);
    assert(strcmp(decoded.package_checksum, stage.package_checksum) == 0);
    assert(strcmp(decoded.image_checksum, stage.image_checksum) == 0);
    assert(strcmp(decoded.version, stage.version) == 0);
    assert(strcmp(decoded.board, stage.board) == 0);
    assert(strcmp(decoded.target, stage.target) == 0);
}

static void test_slot_validation(void) {
    h2_loader_metadata_t stage = valid_stage();
    h2_loader_metadata_t partition = {0};
    assert(h2_loader_metadata_validate(
               H2_LOADER_METADATA_SLOT_STAGE, &stage) == H2_PAL_OK);
    assert(h2_loader_metadata_from_stage(&stage, &partition) == H2_PAL_OK);
    assert(h2_loader_metadata_validate(
               H2_LOADER_METADATA_SLOT_PARTITION_1, &partition) == H2_PAL_OK);
    assert(h2_loader_metadata_image_equal(&stage, &partition));

    partition.package_checksum[0] = '\0';
    partition.package_size = 0u;
    assert(h2_loader_metadata_validate(
               H2_LOADER_METADATA_SLOT_PARTITION_2, &partition) == H2_PAL_OK);
    assert(h2_loader_metadata_image_equal(&stage, &partition));

    stage.package_checksum[0] = '\0';
    assert(h2_loader_metadata_validate(
               H2_LOADER_METADATA_SLOT_STAGE, &stage) == H2_PAL_ERR_INVALID_ARG);
    stage.valid = 0;
    assert(h2_loader_metadata_validate(
               H2_LOADER_METADATA_SLOT_STAGE, &stage) == H2_PAL_OK);
}

static void test_decoder_rejects_wrong_slot_and_corruption(void) {
    const h2_loader_metadata_t stage = valid_stage();
    h2_loader_metadata_t decoded = {0};
    uint8_t encoded[1024];
    size_t encoded_len = 0u;
    assert(h2_loader_metadata_encode(
               H2_LOADER_METADATA_SLOT_STAGE,
               &stage,
               encoded,
               sizeof(encoded),
               &encoded_len) == H2_PAL_OK);
    assert(h2_loader_metadata_decode(
               H2_LOADER_METADATA_SLOT_PARTITION_1,
               encoded,
               encoded_len,
               &decoded) == H2_PAL_ERR_INVALID_ARG);
    encoded[0] ^= UINT8_C(0xff);
    assert(h2_loader_metadata_decode(
               H2_LOADER_METADATA_SLOT_STAGE,
               encoded,
               encoded_len,
               &decoded) == H2_PAL_ERR_INVALID_ARG);
}

static void test_preference_commit_is_atomic(void) {
    static const h2_pal_pref_vtable_t pref_vtable = {
        .open = metadata_pref_open,
    };
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = metadata_alloc,
        .free = metadata_free,
    };
    metadata_pref_fixture_t fixture = {0};
    const h2_pal_pref_api_t pref = {
        .user = &fixture,
        .vtable = &pref_vtable,
    };
    const h2_pal_mem_api_t mem = {
        .vtable = &mem_vtable,
    };
    const h2_loader_metadata_t stage = valid_stage();
    h2_loader_metadata_t read = {0};
    int present = 0;
    fixture.set_blob_result = H2_PAL_OK;
    fixture.remove_result = H2_PAL_OK;

    assert(h2_loader_metadata_write(
               &pref, H2_LOADER_METADATA_SLOT_STAGE, &stage) == H2_PAL_OK);
    assert(fixture.commits == 1u);
    assert(h2_loader_metadata_read(
               &pref,
               &mem,
               H2_LOADER_METADATA_SLOT_STAGE,
               &read,
               &present) == H2_PAL_OK);
    assert(present == 1);
    assert(h2_loader_metadata_image_equal(&stage, &read));

    fixture.set_blob_result = H2_PAL_ERR_WRITE;
    h2_loader_metadata_t invalid = {0};
    assert(h2_loader_metadata_write(
               &pref, H2_LOADER_METADATA_SLOT_STAGE, &invalid) ==
           H2_PAL_ERR_WRITE);
    assert(h2_loader_metadata_read(
               &pref,
               &mem,
               H2_LOADER_METADATA_SLOT_STAGE,
               &read,
               &present) == H2_PAL_OK);
    assert(present == 1 && read.valid == 1);
    fixture.set_blob_result = H2_PAL_OK;

    fixture.commit_result = H2_PAL_ERR_WRITE;
    assert(h2_loader_metadata_write(
               &pref, H2_LOADER_METADATA_SLOT_STAGE, &invalid) ==
           H2_PAL_ERR_WRITE);
    assert(h2_loader_metadata_read(
               &pref,
               &mem,
               H2_LOADER_METADATA_SLOT_STAGE,
               &read,
               &present) == H2_PAL_OK);
    assert(present == 1 && read.valid == 1);

    fixture.commit_result = H2_PAL_OK;
    fixture.remove_result = H2_PAL_ERR_WRITE;
    assert(h2_loader_metadata_clear(
               &pref, H2_LOADER_METADATA_SLOT_STAGE) == H2_PAL_ERR_WRITE);
    assert(h2_loader_metadata_read(
               &pref,
               &mem,
               H2_LOADER_METADATA_SLOT_STAGE,
               &read,
               &present) == H2_PAL_OK);
    assert(present == 1 && read.valid == 1);
    fixture.remove_result = H2_PAL_OK;
    assert(h2_loader_metadata_clear(
               &pref, H2_LOADER_METADATA_SLOT_STAGE) == H2_PAL_OK);
    assert(h2_loader_metadata_read(
               &pref,
               &mem,
               H2_LOADER_METADATA_SLOT_STAGE,
               &read,
               &present) == H2_PAL_OK);
    assert(present == 0 && read.valid == 0);
}

int main(void) {
    test_slot_keys();
    test_round_trip_preserves_complete_identity();
    test_slot_validation();
    test_decoder_rejects_wrong_slot_and_corruption();
    test_preference_commit_is_atomic();
    puts("h2loader metadata tests passed");
    return 0;
}
