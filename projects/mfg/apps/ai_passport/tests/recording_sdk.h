#ifndef H2_PASSPORT_RECORDING_SDK_H
#define H2_PASSPORT_RECORDING_SDK_H

#include <stdint.h>

/* The host test controls only SDK partition/image discovery. Encryption uses
 * the production wolfCrypt PAL provider. */
typedef int esp_err_t;
typedef enum esp_partition_type {
  ESP_PARTITION_TYPE_APP = 0,
} esp_partition_type_t;
typedef enum esp_partition_subtype {
  ESP_PARTITION_SUBTYPE_APP_OTA_1 = 0x11,
} esp_partition_subtype_t;
typedef enum esp_image_load_mode {
  ESP_IMAGE_VERIFY_SILENT = 1,
} esp_image_load_mode_t;

enum { ESP_OK = 0, ESP_FAIL = -1 };

typedef struct esp_partition {
  esp_partition_type_t type;
  esp_partition_subtype_t subtype;
  uint32_t address;
  uint32_t size;
} esp_partition_t;

typedef struct esp_partition_pos {
  uint32_t offset;
  uint32_t size;
} esp_partition_pos_t;

typedef struct esp_image_metadata {
  uint32_t start_addr;
  uint32_t image_len;
} esp_image_metadata_t;

const esp_partition_t* esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char* label);
const esp_partition_t* esp_ota_get_running_partition(void);
esp_err_t esp_image_verify(esp_image_load_mode_t mode,
                           const esp_partition_pos_t* position,
                           esp_image_metadata_t* metadata);

#endif
