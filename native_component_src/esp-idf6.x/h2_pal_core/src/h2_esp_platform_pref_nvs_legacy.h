#ifndef H2_ESP_PLATFORM_PREF_NVS_LEGACY_H
#define H2_ESP_PLATFORM_PREF_NVS_LEGACY_H

#include "h2_esp_platform_pref_store.h"

int h2_esp_pref_legacy_copy(h2_esp_pref_store_t *store);
int h2_esp_pref_legacy_cleanup(void);

#endif
