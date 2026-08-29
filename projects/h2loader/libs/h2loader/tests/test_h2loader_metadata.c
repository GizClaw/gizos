#include "h2_loader_metadata.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_IMAGE_SHA256 \
    "abababababababababababababababababababababababababababababababab"
#define TEST_PACKAGE_SHA256 TEST_IMAGE_SHA256

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

int main(void) {
    test_slot_keys();
    test_round_trip_preserves_complete_identity();
    test_slot_validation();
    test_decoder_rejects_wrong_slot_and_corruption();
    puts("h2loader metadata tests passed");
    return 0;
}
