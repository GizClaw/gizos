#ifndef H2_TEST_PREF_NVS_H
#define H2_TEST_PREF_NVS_H

#include "esp_attr.h"
#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
#define ESP_ERR_NVS_INVALID_NAME 0x1106
#define ESP_ERR_NVS_KEY_TOO_LONG 0x1109
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode,
                   nvs_handle_t* out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value,
                       size_t* length);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

#endif
