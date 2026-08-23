#include "h2_esp_platform_pref_store.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void expect_value(h2_esp_pref_store_t *store, const char *key,
                         h2_pal_pref_entry_type_t type, const void *expected,
                         size_t expected_size) {
    uint8_t *value = NULL;
    size_t size = 0u;
    assert(h2_esp_pref_store_get(store, "suite/ns", key, type, &value, &size) ==
           H2_PAL_OK);
    assert(size == expected_size);
    assert(expected_size == 0u || memcmp(value, expected, expected_size) == 0);
    free(value);
}

int main(void) {
    char root[128];
    h2_esp_pref_store_t store = {0};
    h2_esp_pref_store_entry_t *entries = NULL;
    size_t count = 0u;
    uint8_t blob[] = {0u, 1u, 2u, 0xffu};
    uint8_t u32[] = {0x78u, 0x56u, 0x34u, 0x12u};
    uint8_t boolean = 1u;
    uint8_t replacement[] = {9u, 8u, 7u};
    struct stat before;
    struct stat after;
    char record_path[512];
    char namespace_path[512];
    char marker[16];

    snprintf(root, sizeof(root), "/tmp/h2-pref-store-%ld", (long)getpid());
    assert(mkdir(root, 0700) == 0);
    store.base_path = root;
    store.committed_budget = 128u * 1024u;
    {
        FILE *file;
        snprintf(record_path, sizeof(record_path), "%s/.migration.new", root);
        file = fopen(record_path, "wb");
        assert(file != NULL);
        assert(fclose(file) == 0);
        snprintf(record_path, sizeof(record_path), "%s/.unrelated.new", root);
        file = fopen(record_path, "wb");
        assert(file != NULL);
        assert(fclose(file) == 0);
    }
    assert(h2_esp_pref_store_prepare(&store) == H2_PAL_OK);
    snprintf(record_path, sizeof(record_path), "%s/.migration.new", root);
    assert(access(record_path, F_OK) != 0);
    snprintf(record_path, sizeof(record_path), "%s/.unrelated.new", root);
    assert(access(record_path, F_OK) == 0);
    assert(h2_esp_pref_store_set(&store, "suite/ns", "blob/key",
                                 H2_PAL_PREF_ENTRY_BLOB, blob,
                                 sizeof(blob)) == H2_PAL_OK);
    expect_value(&store, "blob/key", H2_PAL_PREF_ENTRY_BLOB, blob,
                 sizeof(blob));
    assert(h2_esp_pref_store_set(&store, "suite/ns", "string",
                                 H2_PAL_PREF_ENTRY_STRING, "hello", 5u) ==
           H2_PAL_OK);
    expect_value(&store, "string", H2_PAL_PREF_ENTRY_STRING, "hello", 5u);
    assert(h2_esp_pref_store_set(&store, "suite/ns", "u32",
                                 H2_PAL_PREF_ENTRY_U32, u32, sizeof(u32)) ==
           H2_PAL_OK);
    assert(h2_esp_pref_store_set(&store, "suite/ns", "bool",
                                 H2_PAL_PREF_ENTRY_BOOL, &boolean,
                                 sizeof(boolean)) == H2_PAL_OK);
    assert(h2_esp_pref_store_list(&store, "suite/ns", &entries, &count) ==
           H2_PAL_OK);
    assert(count == 4u);
    free(entries);

    snprintf(namespace_path, sizeof(namespace_path), "%s/73756974652f6e73", root);
    snprintf(record_path, sizeof(record_path), "%s/626c6f622f6b6579", namespace_path);
    {
        char recognized_temp[512];
        char unrelated_temp[512];
        FILE *file;
        snprintf(recognized_temp, sizeof(recognized_temp), "%s/74656d70.new",
                 namespace_path);
        snprintf(unrelated_temp, sizeof(unrelated_temp), "%s/not-pref.new",
                 namespace_path);
        file = fopen(recognized_temp, "wb");
        assert(file != NULL);
        assert(fclose(file) == 0);
        file = fopen(unrelated_temp, "wb");
        assert(file != NULL);
        assert(fclose(file) == 0);
        assert(h2_esp_pref_store_prepare(&store) == H2_PAL_OK);
        assert(access(recognized_temp, F_OK) != 0);
        assert(access(unrelated_temp, F_OK) == 0);
    }
    assert(stat(record_path, &before) == 0);
    assert(h2_esp_pref_store_set(&store, "suite/ns", "blob/key",
                                 H2_PAL_PREF_ENTRY_BLOB, blob,
                                 sizeof(blob)) == H2_PAL_OK);
    assert(stat(record_path, &after) == 0);
    assert(before.st_ino == after.st_ino);

    for (store.test_fault_once = H2_ESP_PREF_STORE_FAULT_WRITE;
         store.test_fault_once <= H2_ESP_PREF_STORE_FAULT_RENAME;
         store.test_fault_once++) {
        h2_esp_pref_store_fault_t fault = store.test_fault_once;
        store.test_fault_once = fault;
        assert(h2_esp_pref_store_set(&store, "suite/ns", "blob/key",
                                     H2_PAL_PREF_ENTRY_BLOB, replacement,
                                     sizeof(replacement)) == H2_PAL_ERR_IO);
        expect_value(&store, "blob/key", H2_PAL_PREF_ENTRY_BLOB, blob,
                     sizeof(blob));
        store.test_fault_once = fault;
    }
    store.test_fault_once = H2_ESP_PREF_STORE_FAULT_NO_SPACE;
    assert(h2_esp_pref_store_set(&store, "suite/ns", "blob/key",
                                 H2_PAL_PREF_ENTRY_BLOB, replacement,
                                 sizeof(replacement)) == H2_PAL_OK);
    assert(store.test_fault_hits == 5u);
    expect_value(&store, "blob/key", H2_PAL_PREF_ENTRY_BLOB, replacement,
                 sizeof(replacement));

    store.committed_budget = 1u;
    assert(h2_esp_pref_store_set(&store, "suite/ns", "over-budget",
                                 H2_PAL_PREF_ENTRY_BLOB, blob,
                                 sizeof(blob)) == H2_PAL_ERR_NO_SPACE);
    store.committed_budget = 128u * 1024u;

    assert(h2_esp_pref_store_write_marker(&store, "migration", "complete") ==
           H2_PAL_OK);
    assert(h2_esp_pref_store_read_marker(&store, "migration", marker,
                                         sizeof(marker)) == H2_PAL_OK);
    assert(strcmp(marker, "complete") == 0);

    {
        FILE *record = fopen(record_path, "r+b");
        int byte;
        assert(record != NULL);
        byte = fgetc(record);
        assert(byte != EOF);
        assert(fseek(record, 0L, SEEK_SET) == 0);
        assert(fputc(byte ^ 0xff, record) != EOF);
        assert(fclose(record) == 0);
        assert(h2_esp_pref_store_get(&store, "suite/ns", "blob/key",
                                     H2_PAL_PREF_ENTRY_BLOB,
                                     (uint8_t **)&entries,
                                     &count) == H2_PAL_ERR_IO);
        assert(h2_esp_pref_store_remove(&store, "suite/ns", "blob/key") ==
               H2_PAL_ERR_IO);
        assert(access(record_path, F_OK) == 0);
        record = fopen(record_path, "r+b");
        assert(record != NULL);
        byte = fgetc(record);
        assert(byte != EOF);
        assert(fseek(record, 0L, SEEK_SET) == 0);
        assert(fputc(byte ^ 0xff, record) != EOF);
        assert(fclose(record) == 0);
    }
    assert(h2_esp_pref_store_remove(&store, "suite/ns", "blob/key") ==
           H2_PAL_OK);
    assert(h2_esp_pref_store_get(&store, "suite/ns", "blob/key",
                                 H2_PAL_PREF_ENTRY_BLOB, (uint8_t **)&entries,
                                 &count) == H2_PAL_ERR_NOT_FOUND);
    assert(h2_esp_pref_store_clear(&store, "suite/ns") == H2_PAL_OK);

    assert(unlink(record_path) != 0);
    snprintf(record_path, sizeof(record_path), "%s/.migration", root);
    assert(unlink(record_path) == 0);
    snprintf(record_path, sizeof(record_path), "%s/.unrelated.new", root);
    assert(unlink(record_path) == 0);
    snprintf(record_path, sizeof(record_path), "%s/not-pref.new",
             namespace_path);
    assert(unlink(record_path) == 0);
    assert(rmdir(namespace_path) == 0);
    assert(rmdir(root) == 0);
    return 0;
}
