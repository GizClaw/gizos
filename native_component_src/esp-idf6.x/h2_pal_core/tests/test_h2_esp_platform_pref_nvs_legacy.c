/* Include the implementation to check the private SDK capacity/output contract
 * as well as the public legacy-copy path, without adding a production API. */
#include "h2_esp_platform_pref_nvs_legacy.c"

#include "esp_heap_caps.h"

#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef H2_ESP_SAFE_IO_SCRATCH_CAPACITY
#define H2_ESP_SAFE_IO_SCRATCH_CAPACITY 16384u
#endif

_Alignas(max_align_t) static uint8_t s_worker_context[1024];
static uint8_t s_scratch[H2_ESP_SAFE_IO_SCRATCH_CAPACITY];
static uint8_t s_value[16384u];
static uint8_t s_manifest[16u + 50u * 170u];
static size_t s_value_size, s_manifest_size;
static size_t s_sdk_capacity;
static int s_scratch_held, s_nvs_locked, s_in_callback;
static int s_fail_acquire, s_null_scratch, s_fail_submit, s_fail_mutex;
static int s_fail_alloc, s_report_oversize;
static esp_err_t s_read_error, s_erase_error, s_commit_error;
static uint8_t* s_internal;
static size_t s_internal_size;
static unsigned s_alloc_calls, s_free_calls, s_acquire_calls, s_sdk_calls;
static unsigned s_open_handles, s_close_calls, s_erase_calls, s_commit_calls;
static int s_legacy_present = 1;

void* heap_caps_malloc(size_t size, unsigned caps) {
  assert(s_scratch_held && s_internal == NULL);
  assert(size > sizeof(s_scratch));
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  ++s_alloc_calls;
  s_internal_size = size;
  if (s_fail_alloc) return NULL;
  s_internal = malloc(size);
  assert(s_internal != NULL);
  return s_internal;
}

void heap_caps_free(void* pointer) {
  assert(s_scratch_held);
  if (pointer == NULL) return;
  assert(pointer == s_internal);
  ++s_free_calls;
  free(pointer);
  s_internal = NULL;
}

h2_pal_result_t h2_esp_platform_safe_io_acquire(uint8_t** out_buffer,
                                                size_t* out_capacity) {
  assert(!s_scratch_held);
  ++s_acquire_calls;
  if (s_fail_acquire) return H2_PAL_ERR_TASK;
  s_scratch_held = 1;
  *out_buffer = s_null_scratch ? NULL : s_scratch;
  *out_capacity = sizeof(s_scratch);
  return H2_PAL_OK;
}

void h2_esp_platform_safe_io_release(void) {
  assert(s_scratch_held && s_internal == NULL);
  s_scratch_held = 0;
}

/* Model the internal worker's copied context. SDK stubs below require all SDK
 * calls, keys and size/handle outputs to stay inside this safe-call phase. */
h2_pal_result_t h2_esp_platform_safe_call(
    h2_esp_platform_safe_call_cb_t callback, void* context, size_t context_size,
    size_t stack_depth) {
  assert(!s_in_callback && stack_depth == 4096u);
  assert(context_size <= sizeof(s_worker_context));
  if (s_fail_submit) return H2_PAL_ERR_TASK;
  memcpy(s_worker_context, context, context_size);
  memset(context, 0xa5, context_size);
  s_in_callback = 1;
  callback(s_worker_context);
  s_in_callback = 0;
  memcpy(context, s_worker_context, context_size);
  return H2_PAL_OK;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  return storage;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout) {
  assert(semaphore != NULL && timeout == portMAX_DELAY && !s_nvs_locked);
  if (s_fail_mutex) return pdFALSE;
  s_nvs_locked = 1;
  return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  assert(semaphore != NULL && s_nvs_locked);
  s_nvs_locked = 0;
  return pdTRUE;
}

static void assert_sdk_pointer(const void* pointer) {
  assert(s_in_callback && s_nvs_locked);
  assert((uintptr_t)pointer >= (uintptr_t)s_worker_context);
  assert((uintptr_t)pointer <
         (uintptr_t)s_worker_context + sizeof(s_worker_context));
}

esp_err_t nvs_flash_init(void) {
  assert(s_in_callback && s_nvs_locked);
  return ESP_OK;
}

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode,
                   nvs_handle_t* out_handle) {
  assert_sdk_pointer(name);
  assert_sdk_pointer(out_handle);
  assert(strcmp(name, "h2pref") == 0);
  assert(mode == NVS_READONLY || mode == NVS_READWRITE);
  if (!s_legacy_present) return ESP_ERR_NVS_NOT_FOUND;
  assert(s_open_handles == 0u);
  ++s_open_handles;
  *out_handle = 1u;
  return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
  assert(s_in_callback && s_nvs_locked && handle == 1u);
  assert(s_open_handles == 1u);
  --s_open_handles;
  ++s_close_calls;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value,
                       size_t* length) {
  const uint8_t* data;
  size_t size;
  assert(handle == 1u);
  assert_sdk_pointer(key);
  assert_sdk_pointer(length);
  ++s_sdk_calls;
  if (!s_legacy_present) return ESP_ERR_NVS_NOT_FOUND;
  if (strcmp(key, "manifest") == 0) {
    data = s_manifest;
    size = s_manifest_size;
  } else if (strcmp(key, "d0001") == 0) {
    data = s_value;
    size = s_value_size;
  } else {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (out_value == NULL) {
    *length = size;
    return ESP_OK;
  }
  assert(s_scratch_held);
  assert(out_value == s_scratch || out_value == s_internal);
  assert(*length <=
         (out_value == s_scratch ? sizeof(s_scratch) : s_internal_size));
  s_sdk_capacity = *length;
  /* Match IDF nvs_get_str_or_blob: an undersized read returns the required
   * length and never supplies a partial blob. */
  if (*length < size) {
    *length = size;
    return ESP_ERR_NVS_INVALID_LENGTH;
  }
  if (s_report_oversize) {
    *length += 1u;
    return ESP_OK;
  }
  *length = size;
  memcpy(out_value, data, size);
  /* A later SDK read failure must never publish staged bytes to the caller. */
  return s_read_error;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) {
  assert(s_in_callback && s_nvs_locked && handle == 1u);
  ++s_erase_calls;
  if (s_erase_error == ESP_OK) s_legacy_present = 0;
  return s_erase_error;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
  assert(s_in_callback && s_nvs_locked && handle == 1u);
  ++s_commit_calls;
  return s_commit_error;
}

static void assert_released(void) {
  assert(!s_scratch_held && !s_nvs_locked && !s_in_callback);
  assert(s_internal == NULL && s_open_handles == 0u);
}

static void assert_unchanged(const uint8_t* output, size_t size) {
  for (size_t i = 0u; i < size; ++i) assert(output[i] == 0xa5u);
}

static void test_blob_sizes(void) {
  static const size_t sizes[] = {0u,    1u,    4096u, 4097u,
                                 8192u, 8193u, 9216u, 16384u};
  uint8_t output[sizeof(s_value) + 2u];
  for (size_t i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    s_value_size = sizes[i];
    size_t length = s_value_size;
    const unsigned allocated = s_alloc_calls, freed = s_free_calls;
    memset(output, 0xa5, sizeof(output));
    assert(pref_nvs_get_blob(1u, "d0001", output + 1u, &length) == ESP_OK);
    assert(length == s_value_size && s_sdk_capacity == s_value_size);
    assert(memcmp(output + 1u, s_value, length) == 0);
    assert(output[0] == 0xa5u && output[length + 1u] == 0xa5u);
    const unsigned large = length > sizeof(s_scratch) ? 1u : 0u;
    assert(s_alloc_calls == allocated + large && s_free_calls == freed + large);
    if (large) assert(s_internal_size == length);
    assert_released();
  }
}

static void test_blob_capacity_and_errors(void) {
  uint8_t output[sizeof(s_value)];
  size_t length = 77u;
  s_value_size = 9216u;
  unsigned allocated = s_alloc_calls, acquired = s_acquire_calls;
  assert(pref_nvs_get_blob(1u, "d0001", NULL, &length) == ESP_OK);
  assert(length == s_value_size && s_alloc_calls == allocated);
  assert(s_acquire_calls == acquired);

  /* Both zero and nonzero insufficient capacities keep caller bytes intact. */
  static const size_t capacities[] = {0u, 1u, 4096u, 8192u, 9215u};
  for (size_t i = 0u; i < sizeof(capacities) / sizeof(capacities[0]); ++i) {
    length = capacities[i];
    memset(output, 0xa5, sizeof(output));
    assert(pref_nvs_get_blob(1u, "d0001", output, &length) ==
           ESP_ERR_NVS_INVALID_LENGTH);
    assert(length == s_value_size && s_sdk_capacity == capacities[i]);
    assert_unchanged(output, sizeof(output));
    assert_released();
  }

  length = sizeof(output);
  s_read_error = ESP_FAIL;
  assert(pref_nvs_get_blob(1u, "d0001", output, &length) == ESP_FAIL);
  assert(length == s_value_size);
  assert_unchanged(output, sizeof(output));
  assert_released();
  s_read_error = ESP_OK;

  length = sizeof(output);
  assert(pref_nvs_get_blob(1u, "missing", output, &length) ==
         ESP_ERR_NVS_NOT_FOUND);
  assert(length == sizeof(output));
  assert_unchanged(output, sizeof(output));
  assert_released();

  s_report_oversize = 1;
  length = sizeof(output);
  assert(pref_nvs_get_blob(1u, "d0001", output, &length) ==
         ESP_ERR_INVALID_SIZE);
  assert(length == sizeof(output) + 1u);
  assert_unchanged(output, sizeof(output));
  assert_released();
  s_report_oversize = 0;

  /* An output capacity larger than the blob remains the capacity seen by IDF.
   */
  s_value_size = 1u;
  length = sizeof(output);
  assert(pref_nvs_get_blob(1u, "d0001", output, &length) == ESP_OK);
  assert(length == 1u && s_sdk_capacity == sizeof(output));
  assert(output[0] == s_value[0]);
  assert_unchanged(output + 1u, sizeof(output) - 1u);
  assert_released();
}

static void test_resource_errors(void) {
  uint8_t output[sizeof(s_value)];
  s_value_size = sizeof(s_value);
  for (unsigned fault = 0u; fault < 5u; ++fault) {
    if (fault == 4u && sizeof(s_scratch) >= sizeof(output)) continue;
    s_fail_acquire = fault == 0u;
    s_null_scratch = fault == 1u;
    s_fail_submit = fault == 2u;
    s_fail_mutex = fault == 3u;
    s_fail_alloc = fault == 4u;
    memset(output, 0xa5, sizeof(output));
    size_t length = sizeof(output);
    const unsigned calls = s_sdk_calls;
    const unsigned allocated = s_alloc_calls, freed = s_free_calls;
    assert(pref_nvs_get_blob(1u, "d0001", output, &length) == ESP_ERR_NO_MEM);
    assert(length == sizeof(output) && s_sdk_calls == calls);
    assert_unchanged(output, sizeof(output));
    assert_released();
    if ((fault == 2u || fault == 3u) && sizeof(output) > sizeof(s_scratch)) {
      assert(s_alloc_calls == allocated + 1u && s_free_calls == freed + 1u);
    }
    s_fail_acquire = s_null_scratch = s_fail_submit = s_fail_mutex =
        s_fail_alloc = 0;
    assert(pref_nvs_get_blob(1u, "d0001", output, &length) == ESP_OK);
    assert(length == sizeof(output) && memcmp(output, s_value, length) == 0);
    assert_released();
  }
  size_t length = 17u;
  const unsigned acquired = s_acquire_calls, calls = s_sdk_calls;
  assert(pref_nvs_get_blob(1u, "d0001", output, NULL) == ESP_ERR_INVALID_ARG);
  assert(pref_nvs_get_blob(1u, NULL, output, &length) == ESP_ERR_INVALID_ARG);
  assert(pref_nvs_get_blob(1u, "", output, &length) == ESP_ERR_INVALID_ARG);
  assert(pref_nvs_get_blob(1u, "0123456789abcdef", output, &length) ==
         ESP_ERR_INVALID_ARG);
  assert(length == 17u && s_acquire_calls == acquired && s_sdk_calls == calls);
  assert_released();
}

static void write_u32(uint8_t* output, uint32_t value) {
  for (unsigned i = 0u; i < 4u; ++i) output[i] = (uint8_t)(value >> (8u * i));
}

static void make_manifest(unsigned count) {
  assert(count <= 50u);
  memset(s_manifest, 0, sizeof(s_manifest));
  memcpy(s_manifest, "H2PM", 4u);
  s_manifest[4] = 1u;
  write_u32(s_manifest + 8u, count);
  write_u32(s_manifest + 12u, 170u);
  s_manifest_size = 16u + count * 170u;
  for (unsigned i = 0u; i < count; ++i) {
    uint8_t* record = s_manifest + 16u + i * 170u;
    memcpy(record, "legacy", 7u);
    snprintf((char*)record + 65u, 97u, "blob%u", i);
    record[162] = H2_PAL_PREF_ENTRY_BLOB;
    record[164] = 1u;
    write_u32(record + 166u, (uint32_t)s_value_size);
  }
  s_legacy_present = 1;
}

static void test_legacy_copy(void) {
  const char* tmp = getenv("TEST_TMPDIR");
  const int saved_cwd = open(".", O_RDONLY);
  char root[64];
  assert(saved_cwd >= 0);
  /* Bazel's absolute temp path may exceed the PAL's device path limit. Keep
   * the isolated fixture there, but pass a short relative path to the PAL. */
  assert(chdir(tmp ? tmp : "/tmp") == 0);
  const int root_length =
      snprintf(root, sizeof(root), "h2-pref-nvs-%ld", (long)getpid());
  assert(root_length > 0 && (size_t)root_length < sizeof(root));
  assert(mkdir(root, 0700) == 0);
  h2_esp_pref_store_t store = {
      .base_path = root,
      .committed_budget = 128u * 1024u,
  };
  const int prepare_rc = h2_esp_pref_io_prepare(&store);
  if (prepare_rc != H2_PAL_OK)
    fprintf(stderr, "legacy store prepare rc=%d base_path_bytes=%zu\n",
            prepare_rc, strlen(root));
  assert(prepare_rc == H2_PAL_OK);
  static const size_t sizes[] = {9216u, 16384u};
  for (size_t i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    s_value_size = sizes[i];
    make_manifest(1u);
    const int rc = h2_esp_pref_legacy_copy(&store);
    if (rc != H2_PAL_OK)
      fprintf(stderr,
              "legacy-copy value=%zu scratch=%zu sdk_capacity=%zu rc=%d\n",
              s_value_size, sizeof(s_scratch), s_sdk_capacity, rc);
    assert(rc == H2_PAL_OK);
    uint8_t* output = NULL;
    size_t length = 0u;
    assert(h2_esp_pref_io_get(&store, "legacy", "blob0", H2_PAL_PREF_ENTRY_BLOB,
                              &output, &length) == H2_PAL_OK);
    assert(length == s_value_size && memcmp(output, s_value, length) == 0);
    free(output);
    assert_released();
    assert(s_legacy_present && s_erase_calls == 0u && s_commit_calls == 0u);
  }

  /* Manifests are whole blobs too, and may also exceed a reduced scratch. */
  s_value_size = 1u;
  make_manifest(50u);
  assert(h2_esp_pref_legacy_copy(&store) == H2_PAL_OK);
  assert_released();

  s_value_size = 9216u;
  make_manifest(1u);
  s_read_error = ESP_FAIL;
  unsigned closed = s_close_calls;
  assert(h2_esp_pref_legacy_copy(&store) == H2_PAL_ERR_IO);
  assert(s_close_calls == closed + 1u && s_legacy_present);
  assert_released();
  s_read_error = ESP_OK;
  if (s_value_size > sizeof(s_scratch)) {
    s_fail_alloc = 1;
    closed = s_close_calls;
    assert(h2_esp_pref_legacy_copy(&store) == H2_PAL_ERR_NO_MEMORY);
    assert(s_close_calls == closed + 1u && s_legacy_present);
    assert_released();
    s_fail_alloc = 0;
  }
  assert(h2_esp_pref_legacy_copy(&store) == H2_PAL_OK);
  assert_released();

  assert(h2_esp_pref_io_clear(&store, "legacy") == H2_PAL_OK);
  assert(rmdir(root) == 0);
  assert(fchdir(saved_cwd) == 0);
  assert(close(saved_cwd) == 0);
  s_erase_error = ESP_FAIL;
  assert(h2_esp_pref_legacy_cleanup() == H2_PAL_ERR_IO);
  assert(s_erase_calls == 1u && s_commit_calls == 0u && s_legacy_present);
  assert_released();
  s_erase_error = ESP_OK;
  s_commit_error = ESP_FAIL;
  assert(h2_esp_pref_legacy_cleanup() == H2_PAL_ERR_IO);
  assert(s_erase_calls == 2u && s_commit_calls == 1u);
  assert_released();
  s_commit_error = ESP_OK;
  s_legacy_present = 1;
  assert(h2_esp_pref_legacy_cleanup() == H2_PAL_OK);
  assert(s_erase_calls == 3u && s_commit_calls == 2u && !s_legacy_present);
  assert_released();
}

int main(void) {
  for (size_t i = 0u; i < sizeof(s_value); ++i)
    s_value[i] = (uint8_t)(i * 37u + i / 251u);
  test_legacy_copy();
  s_legacy_present = 1;
  test_blob_sizes();
  test_blob_capacity_and_errors();
  test_resource_errors();
  return 0;
}
