#include "h2_nfc_type2.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int fail_next_allocation;
static size_t live_allocations;

static void *test_alloc(void *user, size_t size) {
    (void)user;
    if (fail_next_allocation) {
        fail_next_allocation = 0;
        return NULL;
    }
    void *pointer = malloc(size);
    if (pointer != NULL) {
        ++live_allocations;
    }
    return pointer;
}

static void test_free(void *user, void *pointer) {
    (void)user;
    if (pointer != NULL) {
        assert(live_allocations != 0u);
        --live_allocations;
    }
    free(pointer);
}

static const h2_pal_mem_vtable_t mem_vtable = {
    .alloc = test_alloc,
    .free = test_free,
};

static const h2_pal_mem_api_t mem_api = {
    .user = NULL,
    .vtable = &mem_vtable,
};

static h2_nfc_type2_t *create_type2(const uint8_t *uid, uint8_t uid_len) {
    h2_nfc_type2_t *type2 = NULL;
    const h2_nfc_type2_config_t config = {
        .mem = &mem_api,
        .uid = uid,
        .uid_len = uid_len,
        .enable_fast_read = 1,
    };
    assert(h2_nfc_type2_create(&config, &type2) == H2_PAL_OK);
    return type2;
}

static size_t process(
    h2_nfc_type2_t *type2,
    const uint8_t *command,
    size_t command_len,
    uint8_t *response,
    size_t capacity) {
    size_t response_len = 99u;
    assert(h2_nfc_type2_process(
               type2, command, command_len, response, capacity,
               &response_len) == H2_PAL_OK);
    return response_len;
}

static void test_four_byte_uid_and_memory(void) {
    const uint8_t uid[] = {0x04u, 0xa1u, 0xb2u, 0xc3u};
    h2_nfc_type2_t *type2 = create_type2(uid, sizeof(uid));
    const uint8_t ndef[] = {0xd1u, 0x01u, 0x02u, 0x54u, 0x00u, 0x41u};
    assert(h2_nfc_type2_set_ndef(type2, ndef, sizeof(ndef), 1u) == H2_PAL_OK);
    uint32_t revision = 0u;
    assert(h2_nfc_type2_activate(type2, &revision) == H2_PAL_OK);
    assert(revision == 1u);

    uint8_t response[64];
    const uint8_t anticollision[] = {0x93u, 0x20u};
    assert(process(type2, anticollision, sizeof(anticollision), response,
                   sizeof(response)) == 5u);
    assert(memcmp(response, uid, sizeof(uid)) == 0);
    assert(response[4] == (uint8_t)(uid[0] ^ uid[1] ^ uid[2] ^ uid[3]));
    const uint8_t reqa[] = {0x26u};
    assert(process(type2, reqa, sizeof(reqa), response, sizeof(response)) == 2u);
    assert(response[0] == 0x04u);

    const uint8_t read_page3[] = {0x30u, 0x03u};
    assert(process(type2, read_page3, sizeof(read_page3), response,
                   sizeof(response)) == 16u);
    assert(response[0] == 0xe1u && response[1] == 0x10u);
    assert(response[3] == 0x0fu);
    assert(response[4] == 0x03u && response[5] == sizeof(ndef));
    assert(memcmp(&response[6], ndef, sizeof(ndef)) == 0);

    const uint8_t write[] = {0xa2u, 0x04u, 1u, 2u, 3u, 4u};
    assert(process(type2, write, sizeof(write), response, sizeof(response)) ==
           0u);
    const uint8_t compat_write[] = {0xa0u, 0x04u, 1u, 2u, 3u, 4u};
    assert(process(
               type2, compat_write, sizeof(compat_write), response,
               sizeof(response)) == 0u);
    assert(h2_nfc_type2_deactivate(type2) == H2_PAL_OK);
    h2_nfc_type2_destroy(type2);
}

static void test_seven_byte_uid_and_revision_boundary(void) {
    const uint8_t uid[] = {0x04u, 1u, 2u, 3u, 4u, 5u, 6u};
    h2_nfc_type2_t *type2 = create_type2(uid, sizeof(uid));
    const uint8_t first[] = {0x11u};
    const uint8_t second[] = {0x22u, 0x23u};
    assert(h2_nfc_type2_set_ndef(type2, first, sizeof(first), 10u) == H2_PAL_OK);
    uint32_t revision;
    assert(h2_nfc_type2_activate(type2, &revision) == H2_PAL_OK);
    assert(revision == 10u);
    assert(h2_nfc_type2_set_ndef(type2, second, sizeof(second), 11u) == H2_PAL_OK);

    uint8_t response[64];
    const uint8_t read[] = {0x30u, 0x04u};
    assert(process(type2, read, sizeof(read), response, sizeof(response)) == 16u);
    assert(response[0] == 0x03u && response[1] == 1u && response[2] == first[0]);
    assert(h2_nfc_type2_deactivate(type2) == H2_PAL_OK);
    assert(h2_nfc_type2_activate(type2, &revision) == H2_PAL_OK);
    assert(revision == 11u);
    assert(process(type2, read, sizeof(read), response, sizeof(response)) == 16u);
    assert(response[0] == 0x03u && response[1] == 2u);
    assert(memcmp(&response[2], second, sizeof(second)) == 0);

    const uint8_t cl1[] = {0x93u, 0x20u};
    assert(process(type2, cl1, sizeof(cl1), response, sizeof(response)) == 5u);
    assert(response[0] == 0x88u);
    const uint8_t cl2[] = {0x95u, 0x20u};
    assert(process(type2, cl2, sizeof(cl2), response, sizeof(response)) == 5u);
    assert(memcmp(response, &uid[3], 4u) == 0);
    const uint8_t read_header[] = {0x30u, 0x00u};
    assert(process(type2, read_header, sizeof(read_header), response,
                   sizeof(response)) == 16u);
    assert(response[3] == (uint8_t)(0x88u ^ uid[0] ^ uid[1] ^ uid[2]));
    assert(response[8] == (uint8_t)(uid[3] ^ uid[4] ^ uid[5] ^ uid[6]));
    assert(response[9] == 0x48u);

    const uint8_t fast[] = {0x3au, 0x03u, 0x05u};
    assert(process(type2, fast, sizeof(fast), response, sizeof(response)) == 12u);
    const uint8_t halt[] = {0x50u, 0x00u};
    assert(process(type2, halt, sizeof(halt), response, sizeof(response)) == 0u);
    size_t response_len;
    const uint8_t reqa[] = {0x26u};
    assert(h2_nfc_type2_process(
               type2, reqa, sizeof(reqa), response, sizeof(response),
               &response_len) == H2_PAL_ERR_WOULD_BLOCK);
    const uint8_t wupa[] = {0x52u};
    assert(process(type2, wupa, sizeof(wupa), response, sizeof(response)) == 2u);

    const uint8_t malformed_read[] = {0x30u};
    assert(h2_nfc_type2_process(
               type2, malformed_read, sizeof(malformed_read), response,
           sizeof(response), &response_len) == H2_PAL_ERR_FORMAT);
    const uint8_t malformed_select[] = {
        0x93u, 0x70u, 0x88u, uid[0], uid[1], uid[2],
        (uint8_t)(0x88u ^ uid[0] ^ uid[1] ^ uid[2]), 0x00u,
    };
    assert(h2_nfc_type2_process(
               type2, malformed_select, sizeof(malformed_select), response,
               sizeof(response), &response_len) == H2_PAL_ERR_FORMAT);
    const uint8_t out_of_range[] = {0x30u, 0xffu};
    assert(process(type2, out_of_range, sizeof(out_of_range), response,
                   sizeof(response)) == 0u);
    const uint8_t fast_out_of_range[] = {0x3au, 0x3fu, 0x40u};
    assert(process(
               type2, fast_out_of_range, sizeof(fast_out_of_range), response,
               sizeof(response)) == 0u);

    assert(h2_nfc_type2_deactivate(type2) == H2_PAL_OK);
    h2_nfc_type2_destroy(type2);
}

static void test_ndef_boundaries(void) {
    const uint8_t uid[] = {1u, 2u, 3u, 4u};
    h2_nfc_type2_t *type2 = create_type2(uid, sizeof(uid));
    uint8_t ndef[H2_NFC_TYPE2_NDEF_MAX_SIZE];
    memset(ndef, 0x5au, sizeof(ndef));
    assert(h2_nfc_type2_set_ndef(type2, ndef, sizeof(ndef), 1u) == H2_PAL_OK);
    assert(h2_nfc_type2_set_ndef(
               type2, ndef, sizeof(ndef) + 1u, 2u) == H2_PAL_ERR_INVALID_ARG);
    uint32_t revision;
    assert(h2_nfc_type2_activate(type2, &revision) == H2_PAL_OK);
    uint8_t response[16];
    size_t response_len;
    const uint8_t read[] = {0x30u, 0x03u};
    assert(h2_nfc_type2_process(
               type2, read, sizeof(read), response, 15u, &response_len) ==
           H2_PAL_ERR_TRUNCATED);
    const uint8_t read_page4[] = {0x30u, 0x04u};
    assert(process(type2, read_page4, sizeof(read_page4), response,
                   sizeof(response)) == 16u);
    assert(response[0] == 0x03u && response[1] == 0xffu);
    assert(response[2] == 0x04u && response[3] == 0x00u);
    uint8_t fast_response[1008];
    const uint8_t fast[] = {0x3au, 0x04u, 0xffu};
    assert(process(type2, fast, sizeof(fast), fast_response,
                   sizeof(fast_response)) == sizeof(fast_response));
    assert(fast_response[0] == 0x03u && fast_response[1] == 0xffu);
    assert(fast_response[2] == 0x04u && fast_response[3] == 0x00u);
    assert(memcmp(&fast_response[4], ndef, sizeof(fast_response) - 4u) == 0);
    assert(h2_nfc_type2_deactivate(type2) == H2_PAL_OK);
    h2_nfc_type2_destroy(type2);
}

static void test_allocation_failure_preserves_committed_and_staged_data(void) {
    const uint8_t uid[] = {1u, 2u, 3u, 4u};
    const uint8_t committed[] = {0x11u, 0x12u};
    const uint8_t staged[] = {0x21u, 0x22u, 0x23u};
    const uint8_t rejected[] = {0x31u, 0x32u, 0x33u, 0x34u};
    h2_nfc_type2_t *type2 = create_type2(uid, sizeof(uid));

    assert(h2_nfc_type2_set_ndef(
               type2, committed, sizeof(committed), 1u) == H2_PAL_OK);
    fail_next_allocation = 1;
    assert(h2_nfc_type2_set_ndef(
               type2, rejected, sizeof(rejected), 9u) ==
           H2_PAL_ERR_NO_MEMORY);

    uint32_t revision = 0u;
    assert(h2_nfc_type2_activate(type2, &revision) == H2_PAL_OK);
    assert(revision == 1u);
    assert(h2_nfc_type2_set_ndef(type2, staged, sizeof(staged), 2u) ==
           H2_PAL_OK);
    fail_next_allocation = 1;
    assert(h2_nfc_type2_set_ndef(
               type2, rejected, sizeof(rejected), 9u) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(h2_nfc_type2_deactivate(type2) == H2_PAL_OK);
    assert(h2_nfc_type2_activate(type2, &revision) == H2_PAL_OK);
    assert(revision == 2u);

    uint8_t response[16];
    const uint8_t read[] = {0x30u, 0x04u};
    assert(process(type2, read, sizeof(read), response, sizeof(response)) ==
           sizeof(response));
    assert(response[0] == 0x03u && response[1] == sizeof(staged));
    assert(memcmp(&response[2], staged, sizeof(staged)) == 0);
    assert(h2_nfc_type2_deactivate(type2) == H2_PAL_OK);
    h2_nfc_type2_destroy(type2);
}

int main(void) {
    test_four_byte_uid_and_memory();
    test_seven_byte_uid_and_revision_boundary();
    test_ndef_boundaries();
    test_allocation_failure_preserves_committed_and_staged_data();
    assert(live_allocations == 0u);
    return 0;
}
