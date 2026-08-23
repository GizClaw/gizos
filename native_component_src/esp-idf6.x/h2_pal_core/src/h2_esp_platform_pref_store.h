#ifndef H2_ESP_PLATFORM_PREF_STORE_H
#define H2_ESP_PLATFORM_PREF_STORE_H

#include "h2/pal/os/h2_pal_pref.h"

#include <stddef.h>
#include <stdint.h>

typedef enum h2_esp_pref_store_fault {
    H2_ESP_PREF_STORE_FAULT_NONE = 0,
    H2_ESP_PREF_STORE_FAULT_WRITE,
    H2_ESP_PREF_STORE_FAULT_SYNC,
    H2_ESP_PREF_STORE_FAULT_CLOSE,
    H2_ESP_PREF_STORE_FAULT_RENAME,
    H2_ESP_PREF_STORE_FAULT_NO_SPACE,
} h2_esp_pref_store_fault_t;

typedef struct h2_esp_pref_store {
    const char *base_path;
    size_t committed_budget;
    h2_esp_pref_store_fault_t test_fault_once;
    unsigned test_fault_hits;
} h2_esp_pref_store_t;

typedef struct h2_esp_pref_store_entry {
    char key[97];
    h2_pal_pref_entry_type_t type;
    size_t value_size;
} h2_esp_pref_store_entry_t;

int h2_esp_pref_store_prepare(h2_esp_pref_store_t *store);
int h2_esp_pref_store_get(
    h2_esp_pref_store_t *store,
    const char *name_space,
    const char *key,
    h2_pal_pref_entry_type_t expected_type,
    uint8_t **out_value,
    size_t *out_value_size);
int h2_esp_pref_store_set(
    h2_esp_pref_store_t *store,
    const char *name_space,
    const char *key,
    h2_pal_pref_entry_type_t type,
    const void *value,
    size_t value_size);
int h2_esp_pref_store_remove(
    h2_esp_pref_store_t *store,
    const char *name_space,
    const char *key);
int h2_esp_pref_store_clear(
    h2_esp_pref_store_t *store,
    const char *name_space);
int h2_esp_pref_store_list(
    h2_esp_pref_store_t *store,
    const char *name_space,
    h2_esp_pref_store_entry_t **out_entries,
    size_t *out_count);
int h2_esp_pref_store_write_marker(
    h2_esp_pref_store_t *store,
    const char *marker,
    const char *value);
int h2_esp_pref_store_read_marker(
    h2_esp_pref_store_t *store,
    const char *marker,
    char *out_value,
    size_t out_value_size);

#endif
