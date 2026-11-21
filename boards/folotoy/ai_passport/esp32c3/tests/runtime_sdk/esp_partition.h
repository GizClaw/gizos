#ifndef H2_PASSPORT_RUNTIME_TEST_PARTITION_H
#define H2_PASSPORT_RUNTIME_TEST_PARTITION_H
#include <stddef.h>

typedef int esp_err_t;
typedef struct esp_partition {
  size_t size;
} esp_partition_t;
enum {
  ESP_OK = 0,
  ESP_PARTITION_TYPE_DATA = 1,
  ESP_PARTITION_SUBTYPE_ANY = 0xff
};
const esp_partition_t* esp_partition_find_first(int type, int subtype,
                                                const char* label);
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset,
                             void* data, size_t size);
#endif
