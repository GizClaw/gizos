#include "h2_esp_platform_image.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/semphr.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const esp_partition_t s_partitions[] = {
    {.type = 0,
     .subtype = 16,
     .address = 0x10000,
     .size = 65536,
     .label = "ota0"},
    {.type = 0,
     .subtype = 17,
     .address = 0x20000,
     .size = 65536,
     .label = "ota1"},
};
static const esp_partition_t* s_running = &s_partitions[0];
static const esp_partition_t* s_next = &s_partitions[0];
static const esp_partition_t* s_writing;
static uint8_t s_bytes[2][65536];
static uint32_t s_lengths[2] = {64u, 128u};
static esp_ota_img_states_t s_state[2] = {ESP_OTA_IMG_VALID, ESP_OTA_IMG_NEW};
static size_t s_written;
static uint32_t s_finish_native_length = 32000u;
static unsigned s_begins, s_aborts, s_ends, s_selects, s_confirms, s_safe_calls;
static int s_fail_big_alloc, s_end_fails, s_state_error, s_safe_fails,
    s_abort_fails;

static size_t index_of(const esp_partition_t* p) {
  assert(p == &s_partitions[0] || p == &s_partitions[1]);
  return p == &s_partitions[1] ? 1u : 0u;
}
const esp_partition_t* esp_partition_find_first(unsigned type, unsigned subtype,
                                                const char* label) {
  (void)label;
  if (type != ESP_PARTITION_TYPE_APP) return NULL;
  if (subtype == 16u) return &s_partitions[0];
  if (subtype == 17u) return &s_partitions[1];
  return NULL;
}
esp_err_t esp_partition_read(const esp_partition_t* p, size_t offset,
                             void* data, size_t len) {
  assert(offset <= p->size && len <= p->size - offset);
  assert(len <= (s_fail_big_alloc ? 4096u : 16384u));
  memcpy(data, s_bytes[index_of(p)] + offset, len);
  return ESP_OK;
}
esp_err_t esp_image_get_metadata(const esp_partition_pos_t* pos,
                                 esp_image_metadata_t* metadata) {
  const size_t index = pos->offset == s_partitions[1].address ? 1u : 0u;
  assert(pos->size == s_partitions[index].size);
  metadata->image_len = s_lengths[index];
  return s_lengths[index] == 0u ? ESP_FAIL : ESP_OK;
}
const esp_partition_t* esp_ota_get_running_partition(void) { return s_running; }
const esp_partition_t* esp_ota_get_boot_partition(void) { return s_next; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* p) {
  s_next = p;
  ++s_selects;
  return ESP_OK;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t* p,
                                      esp_ota_img_states_t* out) {
  if (s_state_error) return s_state_error;
  *out = s_state[index_of(p)];
  return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) {
  assert(s_running != NULL);
  assert(s_state[index_of(s_running)] == ESP_OTA_IMG_NEW ||
         s_state[index_of(s_running)] == ESP_OTA_IMG_PENDING_VERIFY);
  s_state[index_of(s_running)] = ESP_OTA_IMG_VALID;
  ++s_confirms;
  return ESP_OK;
}
esp_err_t esp_ota_begin(const esp_partition_t* p, size_t size,
                        esp_ota_handle_t* handle) {
  assert(p != s_running && size != 0u && size <= p->size && s_writing == NULL);
  s_writing = p;
  s_written = 0u;
  ++s_begins;
  *handle = s_begins;
  return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* data, size_t len) {
  assert(s_writing && handle == s_begins);
  assert(len <= (s_fail_big_alloc ? 4096u : 16384u));
  memcpy(s_bytes[index_of(s_writing)] + s_written, data, len);
  s_written += len;
  return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle) {
  assert(s_writing && handle == s_begins);
  s_lengths[index_of(s_writing)] = s_finish_native_length;
  s_writing = NULL;
  ++s_ends;
  return s_end_fails ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle) {
  assert(s_writing && handle == s_begins);
  ++s_aborts;
  if (s_abort_fails) return ESP_FAIL;
  s_writing = NULL;
  return ESP_OK;
}
void* heap_caps_calloc(size_t count, size_t len, unsigned caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  assert(count * len <= 1024u);
  return calloc(count, len);
}
void* heap_caps_malloc(size_t len, unsigned caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  assert(len <= 16384u);
  return s_fail_big_alloc && len > 4096u ? NULL : malloc(len);
}
void heap_caps_free(void* p) { free(p); }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  return storage;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout) {
  assert(semaphore && timeout == portMAX_DELAY);
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  assert(semaphore);
  return pdTRUE;
}
h2_pal_result_t h2_esp_platform_safe_call(h2_esp_platform_safe_call_cb_t cb,
                                          void* context, size_t size,
                                          size_t stack) {
  assert(size <= 1024u && stack == 4096u);
  ++s_safe_calls;
  if (s_safe_fails) return H2_PAL_ERR_TASK;
  cb(context);
  return H2_PAL_OK;
}
h2_pal_result_t h2_esp_platform_pref_finalize_migration(void) {
  return H2_PAL_OK;
}
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
void esp_restart(void) { assert(0); }
uint32_t esp_sleep_get_wakeup_causes(void) { return 0u; }
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us) {
  (void)us;
  return ESP_OK;
}
void esp_deep_sleep_start(void) { assert(0); }

static void test_images(void) {
  const h2_pal_image_api_t* image = h2_esp_platform_image_api();
  h2_pal_image_info_t info;
  assert(h2_pal_image_get_info(image, 1u, H2_PAL_IMAGE_VIEW_STORED, &info) ==
         H2_PAL_OK);
  assert(info.capacity == 65536u && info.image_size == 0u);
  assert(!(info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN));
  assert(h2_pal_image_get_info(image, 1u, H2_PAL_IMAGE_VIEW_NATIVE, &info) ==
         H2_PAL_OK);
  assert(info.image_size == 64u && (info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN));
  assert(!(info.flags & H2_PAL_IMAGE_FLAG_WRITABLE));
  assert(!(info.flags & H2_PAL_IMAGE_FLAG_EXECUTION_PARTITION_KNOWN));
  s_lengths[1] = 0u;
  assert(h2_pal_image_get_info(image, 2u, H2_PAL_IMAGE_VIEW_NATIVE, &info) ==
         H2_PAL_OK);
  assert(info.capacity == 65536u &&
         !(info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN));
  assert(info.flags & H2_PAL_IMAGE_FLAG_WRITABLE);
  h2_pal_image_write_request_t request = {2u, 1u, 32769u};
  h2_pal_image_write_request_t bad = request;
  bad.partition_id = 1u;
  assert(h2_pal_image_begin(image, &bad) == H2_PAL_ERR_INVALID_ARG);
  bad.partition_id = 0u;
  assert(h2_pal_image_begin(image, &bad) == H2_PAL_ERR_NOT_FOUND);
  bad = request;
  bad.image_size = UINT64_MAX;
  assert(h2_pal_image_begin(image, &bad) == H2_PAL_ERR_INVALID_ARG);
  bad = request;
  bad.image_size = 0u;
  assert(h2_pal_image_begin(image, &bad) == H2_PAL_ERR_INVALID_ARG);
  bad = request;
  bad.execution_partition_id = 3u;
  assert(h2_pal_image_begin(image, &bad) == H2_PAL_ERR_INVALID_ARG);
  assert(s_begins == 0u && s_aborts == 0u);
  assert(h2_pal_image_begin(image, &request) == H2_PAL_OK);
  assert(h2_pal_image_begin(image, &request) == H2_PAL_ERR_INVALID_STATE);
  assert(s_aborts == 0u);
  uint8_t bytes[32769];
  for (size_t i = 0u; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)i;
  s_fail_big_alloc = 1;
  assert(h2_pal_image_write(image, bytes, sizeof(bytes)) == H2_PAL_OK);
  assert(h2_pal_image_write(image, bytes, 1u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_image_finish(image) == H2_PAL_OK);
  assert(s_ends == 1u && s_aborts == 0u && s_selects == 0u);
  assert(h2_pal_image_get_info(image, 2u, H2_PAL_IMAGE_VIEW_STORED, &info) ==
         H2_PAL_OK);
  assert(info.image_size == sizeof(bytes) &&
         (info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN) &&
         (info.flags & H2_PAL_IMAGE_FLAG_PREPARED));
  /* OTA accepted the whole artifact, while the parser reports a shorter native
   * executable. Padding must remain in destination verification's STORED view.
   */
  assert(s_finish_native_length < sizeof(bytes));
  assert(h2_pal_image_get_info(image, 2u, H2_PAL_IMAGE_VIEW_NATIVE, &info) ==
         H2_PAL_OK);
  assert(info.image_size == s_finish_native_length &&
         (info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN));
  uint8_t readback[32769] = {0};
  assert(h2_pal_image_read(image, 2u, H2_PAL_IMAGE_VIEW_STORED, 0u, readback,
                           sizeof(readback)) == H2_PAL_OK);
  assert(memcmp(bytes, readback, sizeof(bytes)) == 0);
  assert(h2_pal_image_read(image, 2u, H2_PAL_IMAGE_VIEW_NATIVE, UINT64_MAX,
                           readback, 2u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_image_read(image, 2u, H2_PAL_IMAGE_VIEW_STORED, 65536u,
                           readback, 1u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_image_abort(image) == H2_PAL_OK);
  assert(h2_pal_image_abort(image) == H2_PAL_OK && s_aborts == 0u);
  /* Losing the in-memory receipt (also true after reset) does not erase native
   * bytes and cannot establish the total artifact length from native metadata.
   */
  assert(h2_pal_image_get_info(image, 2u, H2_PAL_IMAGE_VIEW_STORED, &info) ==
         H2_PAL_OK);
  assert(info.image_size == 0u && !(info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN));
  assert(!(info.flags & H2_PAL_IMAGE_FLAG_PREPARED));
  assert(h2_pal_image_get_info(image, 2u, H2_PAL_IMAGE_VIEW_NATIVE, &info) ==
         H2_PAL_OK);
  assert(info.image_size == s_finish_native_length &&
         (info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN));
  assert(h2_pal_image_begin(image, &request) == H2_PAL_OK);
  assert(h2_pal_image_finish(image) == H2_PAL_ERR_FORMAT && s_aborts == 1u);
  assert(h2_pal_image_begin(image, &request) == H2_PAL_OK);
  assert(h2_pal_image_write(image, bytes, sizeof(bytes)) == H2_PAL_OK);
  s_end_fails = 1;
  assert(h2_pal_image_finish(image) == H2_PAL_ERR_FORMAT);
  assert(h2_pal_image_abort(image) == H2_PAL_OK && s_aborts == 1u);
  assert(h2_pal_image_finish(image) == H2_PAL_ERR_INVALID_STATE);
  s_end_fails = 0;
  assert(h2_pal_image_begin(image, &request) == H2_PAL_OK);
  s_safe_fails = 1;
  assert(h2_pal_image_abort(image) == H2_PAL_ERR_TASK && s_writing);
  s_safe_fails = 0;
  assert(h2_pal_image_abort(image) == H2_PAL_OK && s_aborts == 2u);
  assert(h2_pal_image_begin(image, &request) == H2_PAL_OK);
  s_abort_fails = 1;
  assert(h2_pal_image_abort(image) == H2_PAL_ERR_IO && s_writing);
  assert(h2_pal_image_begin(image, &request) == H2_PAL_ERR_INVALID_STATE);
  s_abort_fails = 0;
  assert(h2_pal_image_abort(image) == H2_PAL_OK && !s_writing);
  assert(s_safe_calls > 20u);
}

typedef struct boot_inventory {
  unsigned count;
  uint32_t flags[2];
} boot_inventory_t;

static h2_pal_result_t collect_boot_partition(
    void* user, const h2_pal_power_boot_partition_t* partition) {
  boot_inventory_t* inventory = user;
  assert(partition->id == 1u || partition->id == 2u);
  inventory->flags[partition->id - 1u] = partition->flags;
  ++inventory->count;
  return H2_PAL_OK;
}

static void test_power(void) {
  const h2_pal_power_api_t* power = h2_esp_platform_power_api();
  h2_pal_power_capabilities_t capabilities;
  assert(h2_pal_power_get_capabilities(power, &capabilities) == H2_PAL_OK);
  const uint32_t image_capabilities = H2_PAL_POWER_CAPABILITY_IMAGE_STATE |
                                      H2_PAL_POWER_CAPABILITY_PREPARE_BOOT |
                                      H2_PAL_POWER_CAPABILITY_CONFIRM_IMAGE;
  assert((capabilities.flags & image_capabilities) == image_capabilities);
  h2_pal_power_native_evidence_t evidence;
  for (unsigned policy = H2_PAL_POWER_BOOT_POLICY_EXISTING;
       policy <= H2_PAL_POWER_BOOT_POLICY_TRIAL_WITH_RECOVERY; ++policy) {
    const h2_pal_power_boot_request_t request = {
        2u, (h2_pal_power_boot_policy_t)policy};
    memset(&evidence, 0xff, sizeof(evidence));
    assert(h2_pal_power_prepare_next_boot_partition(power, &request,
                                                    &evidence) == H2_PAL_OK);
    assert(evidence.format == 0u && evidence.size == 0u && s_selects == 0u);
  }
  assert(h2_pal_power_set_next_boot_partition(power, 1u) == H2_PAL_OK &&
         s_selects == 0u);
  assert(h2_pal_power_set_next_boot_partition(power, 2u) == H2_PAL_OK &&
         s_selects == 1u);
  h2_pal_power_image_confirmation_t confirmation;
  for (unsigned state = ESP_OTA_IMG_NEW; state <= ESP_OTA_IMG_UNDEFINED;
       ++state) {
    s_state[0] = (esp_ota_img_states_t)state;
    boot_inventory_t inventory = {0};
    assert(h2_pal_power_list_boot_partitions(power, collect_boot_partition,
                                             &inventory) == H2_PAL_OK);
    assert(inventory.count == 2u);
    const int bootable =
        state != ESP_OTA_IMG_INVALID && state != ESP_OTA_IMG_ABORTED;
    assert(!!(inventory.flags[0] & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE) ==
           bootable);
    assert(inventory.flags[1] & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE);
    const unsigned before = s_confirms;
    h2_pal_power_image_state_t observed;
    assert(h2_pal_power_get_image_state(power, 1u, &observed) == H2_PAL_OK);
    assert(h2_pal_power_confirm_running_image(power, NULL, &confirmation) ==
           H2_PAL_OK);
    const int pending =
        state == ESP_OTA_IMG_NEW || state == ESP_OTA_IMG_PENDING_VERIFY;
    assert(s_confirms == before + (unsigned)pending);
    assert(confirmation.partition_id == 1u &&
           confirmation.was_pending == pending);
    assert(h2_pal_power_confirm_running_image(power, &evidence,
                                              &confirmation) == H2_PAL_OK);
    assert(s_confirms == before + (unsigned)pending &&
           !confirmation.was_pending);
  }
  /* Factory-written OTA images have no state record yet. Both existing
   * partitions still enumerate, while explicit rollback states stay excluded.
   */
  s_state_error = ESP_ERR_NOT_FOUND;
  boot_inventory_t inventory = {0};
  assert(h2_pal_power_list_boot_partitions(power, collect_boot_partition,
                                           &inventory) == H2_PAL_OK);
  assert(inventory.count == 2u);
  assert(inventory.flags[0] & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE);
  assert(inventory.flags[1] & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE);
  h2_pal_power_image_state_t observed;
  assert(h2_pal_power_get_image_state(power, 2u, &observed) == H2_PAL_OK);
  assert(observed.state == H2_PAL_POWER_IMAGE_STATE_UNKNOWN);
  assert(h2_pal_power_confirm_running_image(power, NULL, &confirmation) ==
         H2_PAL_OK);
  assert(!confirmation.was_pending);
  s_state_error = ESP_FAIL;
  memset(&inventory, 0, sizeof(inventory));
  assert(h2_pal_power_list_boot_partitions(power, collect_boot_partition,
                                           &inventory) == H2_PAL_ERR_IO);
  assert(inventory.count == 0u);
  assert(h2_pal_power_confirm_running_image(power, NULL, &confirmation) ==
         H2_PAL_ERR_IO);
  assert(!confirmation.partition_id);
  s_state_error = 0;
  s_running = NULL;
  assert(h2_pal_power_confirm_running_image(power, NULL, &confirmation) ==
         H2_PAL_ERR_NOT_FOUND);
  s_running = &s_partitions[0];
}

int main(void) {
  test_images();
  test_power();
  return 0;
}
