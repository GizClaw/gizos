#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"

#include "esp_partition.h"
#include "freertos/semphr.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEST_SCRATCH_CAPACITY 4096u
#define TEST_DATA_SIZE (5u * TEST_SCRATCH_CAPACITY + 117u)

static uint8_t s_scratch[TEST_SCRATCH_CAPACITY];
static uint8_t s_flash[32u * 1024u];
static const esp_partition_t s_partition = {
    .type = ESP_PARTITION_TYPE_APP,
    .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
    .size = sizeof(s_flash),
    .label = "app",
};
static SemaphoreHandle_t s_mutex;
static int s_mutex_held, s_scratch_held;
static int s_fail_acquire, s_fail_dispatch;
static size_t s_capacity = sizeof(s_scratch);
static unsigned s_calls, s_fail_partition_call;
static size_t s_offsets[16], s_lengths[16];

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  assert(storage != NULL);
  s_mutex = storage;
  return storage;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout) {
  assert(semaphore == s_mutex && timeout == portMAX_DELAY);
  assert(s_scratch_held && !s_mutex_held);
  s_mutex_held = 1;
  return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  assert(semaphore == s_mutex && s_mutex_held);
  s_mutex_held = 0;
  return pdTRUE;
}

h2_pal_result_t h2_esp_platform_safe_io_acquire(uint8_t** out_buffer,
                                                size_t* out_capacity) {
  assert(!s_scratch_held && !s_mutex_held);
  if (s_fail_acquire) return H2_PAL_ERR_TASK;
  s_scratch_held = 1;
  *out_buffer = s_scratch;
  *out_capacity = s_capacity;
  return H2_PAL_OK;
}

void h2_esp_platform_safe_io_release(void) {
  assert(s_scratch_held && !s_mutex_held);
  s_scratch_held = 0;
}

h2_pal_result_t h2_esp_platform_safe_call(
    h2_esp_platform_safe_call_cb_t callback, void* context, size_t context_size,
    size_t stack_depth) {
  _Alignas(max_align_t) uint8_t copied[1024];
  assert(s_scratch_held && s_mutex_held);
  assert(context_size <= sizeof(copied) && stack_depth == 4096u);
  if (s_fail_dispatch) return H2_PAL_ERR_TASK;
  memcpy(copied, context, context_size);
  memset(context, 0xa5, context_size);
  callback(copied);
  memcpy(context, copied, context_size);
  return H2_PAL_OK;
}

const esp_partition_t* esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char* label) {
  assert(label == NULL);
  return type == s_partition.type && subtype == s_partition.subtype
             ? &s_partition
             : NULL;
}

static esp_err_t record_io(const esp_partition_t* partition, size_t offset,
                           const void* data, size_t len) {
  assert(partition == &s_partition && s_scratch_held && s_mutex_held);
  assert(data == s_scratch && len > 0u && len <= s_capacity);
  assert(offset <= sizeof(s_flash) && len <= sizeof(s_flash) - offset);
  assert(s_calls < sizeof(s_offsets) / sizeof(s_offsets[0]));
  s_offsets[s_calls] = offset;
  s_lengths[s_calls] = len;
  ++s_calls;
  return s_calls == s_fail_partition_call ? ESP_ERR_INVALID_ARG : ESP_OK;
}

esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset,
                             void* data, size_t len) {
  esp_err_t rc = record_io(partition, offset, data, len);
  if (rc == ESP_OK) memcpy(data, s_flash + offset, len);
  return rc;
}

esp_err_t esp_partition_write(const esp_partition_t* partition, size_t offset,
                              const void* data, size_t len) {
  esp_err_t rc = record_io(partition, offset, data, len);
  if (rc == ESP_OK) memcpy(s_flash + offset, data, len);
  return rc;
}

esp_err_t esp_partition_erase_range(const esp_partition_t* partition,
                                    size_t offset, size_t len) {
  (void)partition;
  (void)offset;
  (void)len;
  assert(0 && "this regression exercises disk read/write");
  return ESP_ERR_INVALID_ARG;
}

static void reset_io(void) {
  assert(!s_mutex_held && !s_scratch_held);
  s_calls = 0u;
  s_fail_partition_call = 0u;
  s_fail_acquire = s_fail_dispatch = 0;
  s_capacity = sizeof(s_scratch);
}

static void expect_chunks(size_t offset) {
  assert(s_calls == 6u);
  for (unsigned i = 0u; i < s_calls; ++i) {
    assert(s_offsets[i] == offset + (size_t)i * TEST_SCRATCH_CAPACITY);
    assert(s_lengths[i] == (i == 5u ? 117u : TEST_SCRATCH_CAPACITY));
  }
  assert(!s_mutex_held && !s_scratch_held);
}

int main(void) {
  static uint8_t value[TEST_DATA_SIZE], read[TEST_DATA_SIZE];
  const h2_pal_disk_api_t* disk = h2_esp_platform_disk_api();
  const size_t offset = 31u;
  for (size_t i = 0u; i < sizeof(value); ++i)
    value[i] = (uint8_t)(i * 43u + i / 127u);
  memset(s_flash, 0x7d, sizeof(s_flash));

  assert(h2_pal_disk_write(disk, 2u, offset, value, sizeof(value)) ==
         H2_PAL_OK);
  expect_chunks(offset);
  assert(memcmp(s_flash + offset, value, sizeof(value)) == 0);
  assert(s_flash[offset - 1u] == 0x7d);
  assert(s_flash[offset + sizeof(value)] == 0x7d);
  reset_io();
  assert(h2_pal_disk_read(disk, 2u, offset, read, sizeof(read)) == H2_PAL_OK);
  expect_chunks(offset);
  assert(memcmp(read, value, sizeof(value)) == 0);

  reset_io();
  assert(h2_pal_disk_read(disk, 2u, sizeof(s_flash), NULL, 0u) == H2_PAL_OK);
  assert(h2_pal_disk_write(disk, 2u, sizeof(s_flash), NULL, 0u) == H2_PAL_OK);
  assert(h2_pal_disk_read(disk, 2u, sizeof(s_flash), read, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_disk_write(disk, 2u, sizeof(s_flash), value, 1u) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_disk_read(disk, 99u, 0u, read, 1u) == H2_PAL_ERR_NOT_FOUND);
  assert(s_calls == 0u);

  s_capacity = 0u;
  assert(h2_pal_disk_read(disk, 2u, offset, read, sizeof(read)) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(h2_pal_disk_write(disk, 2u, offset, value, sizeof(value)) ==
         H2_PAL_ERR_NO_MEMORY);
  reset_io();
  s_fail_partition_call = 2u;
  assert(h2_pal_disk_read(disk, 2u, offset, read, sizeof(read)) ==
         H2_PAL_ERR_IO);
  assert(s_calls == 2u);
  reset_io();
  s_fail_partition_call = 2u;
  assert(h2_pal_disk_write(disk, 2u, offset, value, sizeof(value)) ==
         H2_PAL_ERR_IO);
  assert(s_calls == 2u);

  reset_io();
  s_fail_dispatch = 1;
  assert(h2_pal_disk_read(disk, 2u, offset, read, sizeof(read)) ==
         H2_PAL_ERR_TASK);
  assert(s_calls == 0u);
  reset_io();
  s_fail_acquire = 1;
  assert(h2_pal_disk_write(disk, 2u, offset, value, sizeof(value)) ==
         H2_PAL_ERR_TASK);
  assert(s_calls == 0u);
  reset_io();
  assert(h2_pal_disk_read(disk, 2u, offset, read, sizeof(read)) == H2_PAL_OK);
  expect_chunks(offset);
  assert(memcmp(read, value, sizeof(value)) == 0);
  return 0;
}
