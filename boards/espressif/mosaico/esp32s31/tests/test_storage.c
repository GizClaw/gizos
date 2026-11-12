#include "h2_mosaico_storage.h"
#include "h2_mosaico_storage_test_sdk.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

static const esp_partition_t kPartitions[] = {
    {ESP_PARTITION_TYPE_DATA, 2, 0x9000u, 0x6000u, "nvs"},
    {ESP_PARTITION_TYPE_APP, 0, 0x20000u, 0x700000u, "main"},
    {ESP_PARTITION_TYPE_APP, 1, 0x720000u, 0x300000u, "loader"},
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, 0xA20000u,
     0x200000u, "data"},
};

struct iterator {
  size_t index;
};

static unsigned s_iterators;
static bool s_read_fail, s_bad_image, s_bad_fs;
static uint8_t s_magic = ESP_IMAGE_HEADER_MAGIC;
static spi_nand_flash_device_t s_device;
static spi_nand_flash_device_t* s_handle;
static esp_err_t s_init_result, s_close_result, s_usage_result;
static unsigned s_initializations, s_closes;

esp_err_t esp_flash_get_size(void* chip, uint32_t* capacity) {
  assert(chip == NULL);
  *capacity = 16u * 1024u * 1024u;
  return ESP_OK;
}

esp_partition_iterator_t esp_partition_find(int type, int subtype,
                                            const char* label) {
  assert(type == ESP_PARTITION_TYPE_ANY &&
         subtype == ESP_PARTITION_SUBTYPE_ANY && label == NULL);
  esp_partition_iterator_t iterator = malloc(sizeof(*iterator));
  assert(iterator != NULL);
  iterator->index = 0u;
  ++s_iterators;
  return iterator;
}

const esp_partition_t* esp_partition_get(esp_partition_iterator_t iterator) {
  return &kPartitions[iterator->index];
}

esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator) {
  if (++iterator->index < 4u) return iterator;
  esp_partition_iterator_release(iterator);
  return NULL;
}

void esp_partition_iterator_release(esp_partition_iterator_t iterator) {
  free(iterator);
  --s_iterators;
}

esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset,
                             void* out, size_t size) {
  assert(partition->type == ESP_PARTITION_TYPE_APP && offset == 0u &&
         size == 1u);
  *(uint8_t*)out = s_magic;
  return s_read_fail ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_image_get_metadata(const esp_partition_pos_t* partition,
                                 esp_image_metadata_t* image) {
  image->image_len = s_bad_image ? partition->size + 1u : 0x180000u;
  return ESP_OK;
}

esp_err_t esp_littlefs_info(const char* label, size_t* total, size_t* used) {
  assert(label != NULL);
  *total = 0x200000u;
  *used = s_bad_fs ? *total + 1u : 0x80000u;
  return ESP_OK;
}

spi_nand_flash_device_t* bsp_nand_flash_get_handle(void) { return s_handle; }

esp_err_t bsp_nand_flash_init(const void* config,
                              spi_nand_flash_device_t** handle) {
  assert(config == NULL && s_handle == NULL);
  ++s_initializations;
  if (s_init_result == ESP_OK) *handle = s_handle = &s_device;
  return s_init_result;
}

esp_err_t bsp_nand_flash_deinit(void) {
  ++s_closes;
  if (s_close_result == ESP_OK) s_handle = NULL;
  return s_close_result;
}

esp_err_t h2_spi_nand_flash_get_usage(spi_nand_flash_device_t* handle,
                                      uint64_t* used, uint64_t* total) {
  assert(handle == &s_device);
  *used = 8192u;
  *total = 64u * 1024u * 1024u;
  return s_usage_result;
}

int main(void) {
  uint64_t used, total;
  assert(h2_mosaico_storage_internal(&used, &total) == H2_PAL_OK);
  assert(used == 0x9000u + 0x6000u + 2u * 0x180000u + 0x80000u);
  assert(total == 16u * 1024u * 1024u && s_iterators == 0u);
  s_magic = 0xFFu;
  assert(h2_mosaico_storage_internal(&used, &total) == H2_PAL_OK);
  assert(used == 0x9000u + 0x6000u + 0x80000u);
  s_magic = 0x12u;
  assert(h2_mosaico_storage_internal(&used, &total) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(used == 0u && total == 0u && s_iterators == 0u);
  s_magic = ESP_IMAGE_HEADER_MAGIC;
  s_read_fail = true;
  assert(h2_mosaico_storage_internal(&used, &total) == H2_PAL_ERR_IO);
  assert(used == 0u && total == 0u && s_iterators == 0u);
  s_read_fail = false;
  s_bad_image = true;
  assert(h2_mosaico_storage_internal(&used, &total) ==
         H2_PAL_ERR_INVALID_STATE);
  s_bad_image = false;
  s_bad_fs = true;
  assert(h2_mosaico_storage_internal(&used, &total) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(s_iterators == 0u);

  s_handle = &s_device;
  assert(h2_mosaico_storage_nand(&used, &total) == H2_PAL_OK);
  assert(used == 8192u && total == 64u * 1024u * 1024u);
  assert(h2_mosaico_storage_deinit() == H2_PAL_OK);
  assert(s_closes == 0u && s_initializations == 0u);
  s_handle = NULL;
  s_init_result = ESP_FAIL;
  assert(h2_mosaico_storage_nand(&used, &total) == H2_PAL_ERR_IO);
  assert(used == 0u && total == 0u);
  s_init_result = ESP_OK;
  assert(h2_mosaico_storage_nand(&used, &total) == H2_PAL_OK);
  assert(s_initializations == 2u);
  s_usage_result = ESP_ERR_TIMEOUT;
  assert(h2_mosaico_storage_nand(&used, &total) == H2_PAL_ERR_TIMEOUT);
  assert(used == 0u && total == 0u);
  s_close_result = ESP_FAIL;
  assert(h2_mosaico_storage_deinit() == H2_PAL_ERR_IO);
  assert(s_handle == &s_device);
  s_close_result = ESP_OK;
  assert(h2_mosaico_storage_deinit() == H2_PAL_OK);
  assert(s_closes == 2u && s_handle == NULL);
  assert(h2_mosaico_storage_deinit() == H2_PAL_OK && s_closes == 2u);
  return 0;
}
