#ifndef H2_TEST_EXTFLASH_PARTITION_H
#define H2_TEST_EXTFLASH_PARTITION_H
#include "esp_err.h"
#include <stddef.h>
typedef struct {
  size_t size;
} esp_partition_t;
esp_err_t esp_partition_read(const esp_partition_t*, size_t, void*, size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t*, size_t, size_t);
#endif
