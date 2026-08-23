#ifndef H2_ESP_PLATFORM_PREF_MIGRATION_H
#define H2_ESP_PLATFORM_PREF_MIGRATION_H

#include "h2_esp_platform_pref_store.h"

int h2_esp_pref_migration_prepare(h2_esp_pref_store_t *store);
int h2_esp_pref_migration_finalize(void);
h2_pal_result_t h2_esp_platform_pref_finalize_migration(void);

#endif
