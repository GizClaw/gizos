#ifndef H2_TEST_ESP_APP_DESC_H
#define H2_TEST_ESP_APP_DESC_H

typedef struct esp_app_desc {
  char version[32];
} esp_app_desc_t;

const esp_app_desc_t* esp_app_get_description(void);

#endif
