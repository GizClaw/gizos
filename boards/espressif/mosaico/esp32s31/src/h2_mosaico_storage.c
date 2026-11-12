#include "h2_mosaico_storage.h"

#include "bsp/nand_flash.h"
#include "esp_flash.h"
#include "esp_image_format.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "h2_spi_nand_flash_usage.h"

#include <stdbool.h>

static bool s_nand_owned;

static h2_pal_result_t result_of(esp_err_t result) {
  if (result == ESP_OK) return H2_PAL_OK;
  if (result == ESP_ERR_NO_MEM) return H2_PAL_ERR_NO_MEMORY;
  if (result == ESP_ERR_NOT_SUPPORTED) return H2_PAL_ERR_UNSUPPORTED;
  if (result == ESP_ERR_INVALID_ARG) return H2_PAL_ERR_INVALID_ARG;
  if (result == ESP_ERR_INVALID_STATE) return H2_PAL_ERR_INVALID_STATE;
  if (result == ESP_ERR_TIMEOUT) return H2_PAL_ERR_TIMEOUT;
  return H2_PAL_ERR_IO;
}

static esp_err_t partition_used(const esp_partition_t* partition,
                                uint64_t* used) {
  if (partition->type == ESP_PARTITION_TYPE_APP) {
    uint8_t magic;
    esp_err_t result = esp_partition_read(partition, 0u, &magic, sizeof(magic));
    if (result != ESP_OK) return result;
    if (magic == 0xFFu) {
      *used = 0u;
      return ESP_OK;
    }
    if (magic != ESP_IMAGE_HEADER_MAGIC) return ESP_ERR_INVALID_STATE;
    const esp_partition_pos_t position = {partition->address, partition->size};
    esp_image_metadata_t image = {0};
    result = esp_image_get_metadata(&position, &image);
    if (result != ESP_OK) return result;
    if (image.image_len == 0u || image.image_len > partition->size)
      return ESP_ERR_INVALID_STATE;
    *used = image.image_len;
    return ESP_OK;
  }
  if (partition->type == ESP_PARTITION_TYPE_DATA &&
      partition->subtype == ESP_PARTITION_SUBTYPE_DATA_LITTLEFS) {
    size_t total = 0u, allocated = 0u;
    const esp_err_t result =
        esp_littlefs_info(partition->label, &total, &allocated);
    if (result != ESP_OK) return result;
    if (allocated > total || total > partition->size)
      return ESP_ERR_INVALID_STATE;
    *used = allocated;
    return ESP_OK;
  }
  *used = partition->size;
  return ESP_OK;
}

h2_pal_result_t h2_mosaico_storage_internal(uint64_t* used_bytes,
                                            uint64_t* total_bytes) {
  if (used_bytes == NULL || total_bytes == NULL) return H2_PAL_ERR_INVALID_ARG;
  *used_bytes = *total_bytes = 0u;
  uint32_t capacity = 0u;
  esp_err_t result = esp_flash_get_size(NULL, &capacity);
  if (result != ESP_OK) return result_of(result);
  esp_partition_iterator_t iterator = esp_partition_find(
      ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  if (iterator == NULL) return H2_PAL_ERR_NOT_FOUND;
  uint64_t used = 0u;
  uint32_t first_partition = capacity;
  while (iterator != NULL) {
    const esp_partition_t* partition = esp_partition_get(iterator);
    if (partition == NULL || partition->address > capacity ||
        partition->size > capacity - partition->address) {
      esp_partition_iterator_release(iterator);
      return H2_PAL_ERR_INVALID_STATE;
    }
    if (partition->address < first_partition)
      first_partition = partition->address;
    uint64_t allocated = 0u;
    result = partition_used(partition, &allocated);
    if (result != ESP_OK) {
      esp_partition_iterator_release(iterator);
      return result_of(result);
    }
    used += allocated;
    iterator = esp_partition_next(iterator);
  }
  used += first_partition;
  if (capacity == 0u || used > capacity) return H2_PAL_ERR_INVALID_STATE;
  *used_bytes = used;
  *total_bytes = capacity;
  return H2_PAL_OK;
}

h2_pal_result_t h2_mosaico_storage_nand(uint64_t* used_bytes,
                                        uint64_t* total_bytes) {
  if (used_bytes == NULL || total_bytes == NULL) return H2_PAL_ERR_INVALID_ARG;
  *used_bytes = *total_bytes = 0u;
  spi_nand_flash_device_t* handle = bsp_nand_flash_get_handle();
  if (handle == NULL) {
    const esp_err_t initialized = bsp_nand_flash_init(NULL, &handle);
    if (initialized != ESP_OK) return result_of(initialized);
    s_nand_owned = true;
  }
  const esp_err_t result =
      h2_spi_nand_flash_get_usage(handle, used_bytes, total_bytes);
  if (result != ESP_OK) *used_bytes = *total_bytes = 0u;
  return result_of(result);
}

h2_pal_result_t h2_mosaico_storage_deinit(void) {
  if (!s_nand_owned) return H2_PAL_OK;
  const esp_err_t result = bsp_nand_flash_deinit();
  if (result == ESP_OK) s_nand_owned = false;
  return result_of(result);
}
