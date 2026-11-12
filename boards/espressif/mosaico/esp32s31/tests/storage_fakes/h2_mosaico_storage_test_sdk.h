#ifndef H2_MOSAICO_STORAGE_TEST_SDK_H
#define H2_MOSAICO_STORAGE_TEST_SDK_H

#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_TIMEOUT 5
#define ESP_FAIL 6
#define ESP_PARTITION_TYPE_ANY 0xFF
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 0xFF
#define ESP_PARTITION_SUBTYPE_DATA_LITTLEFS 0x83
#define ESP_IMAGE_HEADER_MAGIC 0xE9

typedef struct esp_partition {
  int type;
  int subtype;
  uint32_t address;
  uint32_t size;
  char label[16];
} esp_partition_t;
typedef struct iterator* esp_partition_iterator_t;
typedef struct esp_partition_pos {
  uint32_t offset;
  uint32_t size;
} esp_partition_pos_t;
typedef struct esp_image_metadata {
  uint32_t image_len;
} esp_image_metadata_t;
typedef struct spi_nand_flash_device {
  unsigned marker;
} spi_nand_flash_device_t;

esp_err_t esp_flash_get_size(void* chip, uint32_t* capacity);
esp_partition_iterator_t esp_partition_find(int type, int subtype,
                                            const char* label);
const esp_partition_t* esp_partition_get(esp_partition_iterator_t iterator);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator);
void esp_partition_iterator_release(esp_partition_iterator_t iterator);
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset,
                             void* out, size_t size);
esp_err_t esp_image_get_metadata(const esp_partition_pos_t* partition,
                                 esp_image_metadata_t* image);
esp_err_t esp_littlefs_info(const char* label, size_t* total, size_t* used);
spi_nand_flash_device_t* bsp_nand_flash_get_handle(void);
esp_err_t bsp_nand_flash_init(const void* config,
                              spi_nand_flash_device_t** handle);
esp_err_t bsp_nand_flash_deinit(void);
esp_err_t h2_spi_nand_flash_get_usage(spi_nand_flash_device_t* handle,
                                      uint64_t* used, uint64_t* total);

#endif
