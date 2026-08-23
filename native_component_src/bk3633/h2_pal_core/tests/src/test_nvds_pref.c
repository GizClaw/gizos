#include "fake_nvds.h"

#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void *test_alloc(void *user, size_t len)
{
    (void)user;
    return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len)
{
    (void)user;
    return realloc(ptr, len);
}

static void test_free(void *user, void *ptr)
{
    (void)user;
    free(ptr);
}

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};

static const h2_pal_mem_api_t s_mem = {
    .user = NULL,
    .vtable = &s_mem_vtable,
};

static const h2_bk3633_nvds_pref_entry_t s_entries[] = {
    {"test", "blob", H2_PAL_PREF_ENTRY_BLOB, 0xa0u, 8u},
    {"test", "string", H2_PAL_PREF_ENTRY_STRING, 0xa1u, 16u},
    {"test", "u32", H2_PAL_PREF_ENTRY_U32, 0xa2u, 4u},
    {"test", "i32", H2_PAL_PREF_ENTRY_I32, 0xa3u, 4u},
    {"test", "bool", H2_PAL_PREF_ENTRY_BOOL, 0xa4u, 1u},
    {"other", "value", H2_PAL_PREF_ENTRY_U32, 0xa5u, 4u},
};

static void test_mapping_validation(fake_nvds_t *fake)
{
    h2_bk3633_nvds_pref_entry_t invalid = s_entries[0];
    h2_bk3633_nvds_pref_entry_t duplicates[2] = {
        s_entries[0], s_entries[1],
    };
    invalid.nvds_tag = 0x20u;
    assert(h2_bk3633_nvds_pref_init_with_driver(
        &invalid, 1u, fake_nvds_driver(fake)) == H2_PAL_ERR_INVALID_ARG);
    invalid = s_entries[0];
    invalid.type = H2_PAL_PREF_ENTRY_UNKNOWN;
    assert(h2_bk3633_nvds_pref_init_with_driver(
        &invalid, 1u, fake_nvds_driver(fake)) == H2_PAL_ERR_INVALID_ARG);
    invalid = s_entries[0];
    invalid.max_value_size = 0u;
    assert(h2_bk3633_nvds_pref_init_with_driver(
        &invalid, 1u, fake_nvds_driver(fake)) == H2_PAL_ERR_INVALID_ARG);
    duplicates[1].nvds_tag = duplicates[0].nvds_tag;
    assert(h2_bk3633_nvds_pref_init_with_driver(
        duplicates, 2u, fake_nvds_driver(fake)) == H2_PAL_ERR_INVALID_ARG);
    duplicates[1] = duplicates[0];
    duplicates[1].nvds_tag = 0xa6u;
    assert(h2_bk3633_nvds_pref_init_with_driver(
        duplicates, 2u, fake_nvds_driver(fake)) == H2_PAL_ERR_INVALID_ARG);
}

static h2_pal_pref_namespace_t *open_test_namespace(
    h2_pal_pref_open_mode_t mode)
{
    char transient_name[] = "test";
    h2_pal_pref_namespace_t *name_space = NULL;
    assert(h2_pal_pref_open(
        h2_bk3633_nvds_pref_api(), transient_name, mode, &name_space) ==
        H2_PAL_OK);
    memset(transient_name, 0, sizeof(transient_name));
    assert(name_space != NULL);
    return name_space;
}

static void test_values_and_persistence(fake_nvds_t *fake)
{
    h2_pal_pref_namespace_t *name_space =
        open_test_namespace(H2_PAL_PREF_OPEN_READ_WRITE);
    const uint8_t blob[] = {1u, 2u, 3u};
    void *blob_result = NULL;
    size_t blob_len = 0u;
    char *string_result = NULL;
    uint32_t u32 = 0u;
    int32_t i32 = 0;
    int boolean = 0;
    h2_pal_pref_cursor_t *cursor = NULL;
    h2_pal_pref_entry_t entry;
    size_t iterated = 0u;

    assert(name_space->set_blob(name_space, "blob", blob, sizeof(blob)) == H2_PAL_OK);
    assert(name_space->set_string(name_space, "string", "GizOS") == H2_PAL_OK);
    assert(name_space->set_u32(name_space, "u32", 0x78563412u) == H2_PAL_OK);
    assert(name_space->set_i32(name_space, "i32", -12345) == H2_PAL_OK);
    assert(name_space->set_bool(name_space, "bool", 7) == H2_PAL_OK);
    assert(fake->tags[0xa2u].data[0] == 0x12u);
    assert(fake->tags[0xa2u].data[3] == 0x78u);
    assert(fake->tags[0xa4u].len == 1u && fake->tags[0xa4u].data[0] == 1u);
    assert(name_space->commit(name_space) == H2_PAL_OK);

    assert(name_space->get_blob(
        name_space, &s_mem, "blob", &blob_result, &blob_len) == H2_PAL_OK);
    assert(blob_len == sizeof(blob) && memcmp(blob_result, blob, blob_len) == 0);
    h2_pal_mem_free(&s_mem, blob_result);
    assert(name_space->get_string(
        name_space, &s_mem, "string", &string_result) == H2_PAL_OK);
    assert(strcmp(string_result, "GizOS") == 0);
    h2_pal_mem_free(&s_mem, string_result);
    assert(name_space->get_u32(name_space, "u32", &u32) == H2_PAL_OK);
    assert(u32 == 0x78563412u);
    assert(name_space->get_i32(name_space, "i32", &i32) == H2_PAL_OK);
    assert(i32 == -12345);
    assert(name_space->get_bool(name_space, "bool", &boolean) == H2_PAL_OK);
    assert(boolean == 1);

    while (name_space->iterate(name_space, &cursor, &entry) == H2_PAL_OK) {
        assert(entry.key != NULL);
        ++iterated;
    }
    assert(iterated == 5u && cursor == NULL);
    assert(name_space->iterate_close(name_space, &cursor) == H2_PAL_OK);
    assert(name_space->close(name_space) == H2_PAL_OK);
    assert(name_space->close(name_space) == H2_PAL_OK);

    h2_bk3633_nvds_pref_deinit();
    assert(h2_bk3633_nvds_pref_api() == h2_pal_unsupported_pref_api());
    assert(h2_bk3633_nvds_pref_init_with_driver(
        s_entries,
        sizeof(s_entries) / sizeof(s_entries[0]),
        fake_nvds_driver(fake)) == H2_PAL_OK);
    name_space = open_test_namespace(H2_PAL_PREF_OPEN_READ_ONLY);
    assert(name_space->get_u32(name_space, "u32", &u32) == H2_PAL_OK);
    assert(u32 == 0x78563412u);
    assert(name_space->set_u32(name_space, "u32", 1u) == H2_PAL_ERR_INVALID_STATE);
    assert(name_space->remove(name_space, "u32") == H2_PAL_ERR_INVALID_STATE);
    assert(name_space->clear(name_space) == H2_PAL_ERR_INVALID_STATE);
    assert(name_space->close(name_space) == H2_PAL_OK);
}

static void test_errors_and_handles(fake_nvds_t *fake)
{
    h2_pal_pref_namespace_t *handles[5] = {0};
    uint32_t value = 99u;
    int boolean = 0;
    const uint8_t malformed_u32[] = {1u, 2u, 3u};
    const uint8_t malformed_bool[] = {2u};
    const uint8_t malformed_utf8[] = {0xc0u, 0x80u};
    size_t index;

    for (index = 0u; index < 4u; ++index) {
        assert(h2_pal_pref_open(
            h2_bk3633_nvds_pref_api(),
            "test",
            H2_PAL_PREF_OPEN_READ_WRITE,
            &handles[index]) == H2_PAL_OK);
    }
    assert(h2_pal_pref_open(
        h2_bk3633_nvds_pref_api(),
        "test",
        H2_PAL_PREF_OPEN_READ_WRITE,
        &handles[4]) == H2_PAL_ERR_FULL);
    assert(h2_pal_pref_open(
        h2_bk3633_nvds_pref_api(),
        "missing",
        H2_PAL_PREF_OPEN_READ_WRITE,
        &handles[4]) == H2_PAL_ERR_NOT_FOUND);
    for (index = 0u; index < 4u; ++index) {
        assert(handles[index]->close(handles[index]) == H2_PAL_OK);
    }

    handles[0] = open_test_namespace(H2_PAL_PREF_OPEN_READ_WRITE);
    assert(handles[0]->get_u32(handles[0], "missing", &value) == H2_PAL_ERR_NOT_FOUND);
    assert(handles[0]->get_u32(handles[0], "blob", &value) == H2_PAL_ERR_INVALID_STATE);
    fake_nvds_set_raw(fake, 0xa2u, malformed_u32, sizeof(malformed_u32));
    assert(handles[0]->get_u32(handles[0], "u32", &value) == H2_PAL_ERR_IO);
    fake_nvds_set_raw(fake, 0xa4u, malformed_bool, sizeof(malformed_bool));
    assert(handles[0]->get_bool(handles[0], "bool", &boolean) == H2_PAL_ERR_IO);
    fake_nvds_set_raw(fake, 0xa1u, malformed_utf8, sizeof(malformed_utf8));
    {
        char *string_result = (char *)1;
        assert(handles[0]->get_string(
            handles[0], &s_mem, "string", &string_result) == H2_PAL_ERR_IO);
        assert(string_result == NULL);
    }
    fake->next_put_status = H2_BK3633_NVDS_STATUS_NO_SPACE;
    assert(handles[0]->set_u32(handles[0], "u32", 1u) == H2_PAL_ERR_NO_SPACE);
    fake->next_get_status = H2_BK3633_NVDS_STATUS_CORRUPT;
    assert(handles[0]->get_u32(handles[0], "u32", &value) == H2_PAL_ERR_IO);
    fake->next_del_status = H2_BK3633_NVDS_STATUS_FAIL;
    assert(handles[0]->remove(handles[0], "u32") == H2_PAL_ERR_IO);
    assert(handles[0]->close(handles[0]) == H2_PAL_OK);
}

int main(void)
{
    static fake_nvds_t fake;
    fake_nvds_init(&fake);
    test_mapping_validation(&fake);
    assert(h2_bk3633_nvds_pref_init_with_driver(
        s_entries,
        sizeof(s_entries) / sizeof(s_entries[0]),
        fake_nvds_driver(&fake)) == H2_PAL_OK);
    assert(h2_bk3633_nvds_pref_init_with_driver(
        s_entries,
        sizeof(s_entries) / sizeof(s_entries[0]),
        fake_nvds_driver(&fake)) == H2_PAL_ERR_INVALID_STATE);
    test_values_and_persistence(&fake);
    test_errors_and_handles(&fake);
    {
        h2_pal_pref_namespace_t *name_space =
            open_test_namespace(H2_PAL_PREF_OPEN_READ_WRITE);
        assert(name_space->clear(name_space) == H2_PAL_OK);
        assert(name_space->remove(name_space, "u32") == H2_PAL_ERR_NOT_FOUND);
        assert(name_space->close(name_space) == H2_PAL_OK);
    }
    h2_bk3633_nvds_pref_deinit();
    return 0;
}
