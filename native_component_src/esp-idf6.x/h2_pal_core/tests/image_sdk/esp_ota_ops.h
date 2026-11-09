#ifndef IMAGE_TEST_ESP_OTA_OPS_H
#define IMAGE_TEST_ESP_OTA_OPS_H
#include "esp_partition.h"
typedef unsigned esp_ota_handle_t;
typedef enum {
  ESP_OTA_IMG_NEW,
  ESP_OTA_IMG_PENDING_VERIFY,
  ESP_OTA_IMG_VALID,
  ESP_OTA_IMG_INVALID,
  ESP_OTA_IMG_ABORTED,
  ESP_OTA_IMG_UNDEFINED
} esp_ota_img_states_t;
const esp_partition_t* esp_ota_get_running_partition(void);
const esp_partition_t* esp_ota_get_boot_partition(void);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition);
esp_err_t esp_ota_get_state_partition(const esp_partition_t* partition,
                                      esp_ota_img_states_t* state);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_begin(const esp_partition_t* partition, size_t size,
                        esp_ota_handle_t* handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
#endif
