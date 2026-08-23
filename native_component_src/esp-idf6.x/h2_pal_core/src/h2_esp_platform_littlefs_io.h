#ifndef H2_ESP_PLATFORM_LITTLEFS_IO_H
#define H2_ESP_PLATFORM_LITTLEFS_IO_H

#include "h2_esp_platform_pref_store.h"

int h2_esp_pref_io_prepare(const h2_esp_pref_store_t *store);
int h2_esp_pref_io_get(const h2_esp_pref_store_t *store,
                       const char *name_space, const char *key,
                       h2_pal_pref_entry_type_t expected_type,
                       uint8_t **out_value, size_t *out_value_size);
int h2_esp_pref_io_set(const h2_esp_pref_store_t *store,
                       const char *name_space, const char *key,
                       h2_pal_pref_entry_type_t type, const void *value,
                       size_t value_size);
int h2_esp_pref_io_remove(const h2_esp_pref_store_t *store,
                          const char *name_space, const char *key);
int h2_esp_pref_io_clear(const h2_esp_pref_store_t *store,
                         const char *name_space);
int h2_esp_pref_io_list(const h2_esp_pref_store_t *store,
                        const char *name_space,
                        h2_esp_pref_store_entry_t **out_entries,
                        size_t *out_count);
int h2_esp_pref_io_write_marker(const h2_esp_pref_store_t *store,
                                const char *marker, const char *value);
int h2_esp_pref_io_read_marker(const h2_esp_pref_store_t *store,
                               const char *marker, char *out_value,
                               size_t out_value_size);

#endif
