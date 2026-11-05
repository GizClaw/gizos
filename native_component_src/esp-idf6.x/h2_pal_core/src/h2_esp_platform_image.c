#include "h2_esp_platform_image.h"
#include "h2_esp_platform_safe_call.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

#define IMAGE_STACK_DEPTH 4096u
#define IMAGE_CHUNK_SIZE (16u * 1024u)
#define IMAGE_FALLBACK_CHUNK_SIZE (4u * 1024u)

typedef struct image_writer {
  esp_ota_handle_t handle;
  uint64_t expected;
  uint64_t written;
  uint64_t prepared_size;
  uint32_t partition_id;
  uint32_t prepared_id;
  int active;
} image_writer_t;

typedef enum image_op {
  IMAGE_INFO,
  IMAGE_READ,
  IMAGE_BEGIN,
  IMAGE_WRITE,
  IMAGE_FINISH,
  IMAGE_ABORT,
} image_op_t;

typedef struct image_call {
  image_op_t op;
  h2_pal_image_view_t view;
  h2_pal_image_write_request_t request;
  h2_pal_image_info_t info;
  esp_partition_pos_t position;
  esp_image_metadata_t metadata;
  uint64_t offset;
  uint8_t* data;
  size_t len;
  h2_pal_result_t result;
} image_call_t;

_Static_assert(sizeof(image_call_t) <= 1024u,
               "Image context must fit safe-call storage");

/* Native state and every cache-disabled callback argument stay in internal RAM.
 */
static DRAM_ATTR image_writer_t s_writer;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static const esp_partition_t* image_partition(uint32_t id) {
  if (id != 1u && id != 2u) return NULL;
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  id == 1u ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                           : ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                  NULL);
}

static h2_pal_result_t image_abort_native(void) {
  esp_err_t err = ESP_OK;
  if (s_writer.active) err = esp_ota_abort(s_writer.handle);
  if (err != ESP_OK) return H2_PAL_ERR_IO;
  memset(&s_writer, 0, sizeof(s_writer));
  return err == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static void IRAM_ATTR image_safe_callback(void* context) {
  image_call_t* call = context;
  const esp_partition_t* partition = NULL;
  call->result = H2_PAL_OK;
  if (call->op == IMAGE_INFO || call->op == IMAGE_READ ||
      call->op == IMAGE_BEGIN) {
    partition = image_partition(call->request.partition_id);
    if (partition == NULL) {
      call->result = H2_PAL_ERR_NOT_FOUND;
      return;
    }
  }
  switch (call->op) {
    case IMAGE_INFO: {
      const esp_partition_t* running = esp_ota_get_running_partition();
      call->info.capacity = partition->size;
      call->info.flags = H2_PAL_IMAGE_FLAG_READABLE;
      if (running != NULL && partition->address != running->address)
        call->info.flags |= H2_PAL_IMAGE_FLAG_WRITABLE;
      if (s_writer.prepared_id == call->request.partition_id)
        call->info.flags |= H2_PAL_IMAGE_FLAG_PREPARED;
      if (call->view == H2_PAL_IMAGE_VIEW_STORED) {
        /* Metadata describes the executable, not trailing artifact padding.
         * Only a completed transaction supplies an exact artifact receipt.
         * After reset/abort that length is unknown; never guess from image_len.
         */
        if (s_writer.prepared_id == call->request.partition_id) {
          call->info.image_size = s_writer.prepared_size;
          call->info.flags |= H2_PAL_IMAGE_FLAG_SIZE_KNOWN;
        }
      } else {
        call->position.offset = partition->address;
        call->position.size = partition->size;
        if (esp_image_get_metadata(&call->position, &call->metadata) ==
                ESP_OK &&
            call->metadata.image_len != 0u &&
            call->metadata.image_len <= partition->size) {
          call->info.image_size = call->metadata.image_len;
          call->info.flags |= H2_PAL_IMAGE_FLAG_SIZE_KNOWN;
        }
      }
      break;
    }
    case IMAGE_READ:
      if (call->offset > partition->size ||
          call->len > partition->size - call->offset) {
        call->result = H2_PAL_ERR_INVALID_ARG;
      } else if (call->len != 0u &&
                 esp_partition_read(partition, (size_t)call->offset, call->data,
                                    call->len) != ESP_OK) {
        call->result = H2_PAL_ERR_IO;
      }
      break;
    case IMAGE_BEGIN: {
      const esp_partition_t* running = esp_ota_get_running_partition();
      if (s_writer.active) {
        call->result = H2_PAL_ERR_INVALID_STATE;
        break;
      }
      if (running == NULL || running->address == partition->address ||
          call->request.image_size == 0u ||
          call->request.image_size > partition->size ||
          call->request.image_size > SIZE_MAX ||
          (call->request.execution_partition_id != 1u &&
           call->request.execution_partition_id != 2u)) {
        call->result = H2_PAL_ERR_INVALID_ARG;
        break;
      }
      memset(&s_writer, 0, sizeof(s_writer));
      if (esp_ota_begin(partition, (size_t)call->request.image_size,
                        &s_writer.handle) != ESP_OK) {
        call->result = H2_PAL_ERR_IO;
        break;
      }
      s_writer.expected = call->request.image_size;
      s_writer.partition_id = call->request.partition_id;
      s_writer.active = 1;
      break;
    }
    case IMAGE_WRITE:
      if (!s_writer.active) {
        call->result = H2_PAL_ERR_INVALID_STATE;
      } else if (call->len > s_writer.expected - s_writer.written) {
        call->result = H2_PAL_ERR_INVALID_ARG;
      } else if (call->len != 0u && esp_ota_write(s_writer.handle, call->data,
                                                  call->len) != ESP_OK) {
        call->result = H2_PAL_ERR_IO;
      } else {
        s_writer.written += call->len;
      }
      break;
    case IMAGE_FINISH: {
      if (!s_writer.active) {
        call->result = H2_PAL_ERR_INVALID_STATE;
        break;
      }
      const uint32_t id = s_writer.partition_id;
      const uint64_t stored_size = s_writer.written;
      if (s_writer.written != s_writer.expected) {
        (void)image_abort_native();
        call->result = H2_PAL_ERR_FORMAT;
        break;
      }
      /* OTA end validates the native image (valid in either ESP OTA window)
       * and releases its handle even on failure. Never abort that stale handle.
       */
      const esp_err_t err = esp_ota_end(s_writer.handle);
      memset(&s_writer, 0, sizeof(s_writer));
      call->result = err == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
      if (err == ESP_OK) {
        s_writer.prepared_id = id;
        s_writer.prepared_size = stored_size;
      }
      break;
    }
    case IMAGE_ABORT:
      call->result = image_abort_native();
      break;
  }
}

static h2_pal_result_t image_submit(image_call_t* call) {
  if (call == NULL) return H2_PAL_ERR_NO_MEMORY;
  portENTER_CRITICAL(&s_mutex_init_lock);
  if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
  portEXIT_CRITICAL(&s_mutex_init_lock);
  if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
    return H2_PAL_ERR_TASK;
  h2_pal_result_t rc = h2_esp_platform_safe_call(
      image_safe_callback, call, sizeof(*call), IMAGE_STACK_DEPTH);
  (void)xSemaphoreGive(s_mutex);
  return rc == H2_PAL_OK ? call->result : rc;
}

static image_call_t* image_call_alloc(image_op_t op, size_t remaining) {
  image_call_t* call = heap_caps_calloc(1u, sizeof(*call),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (call == NULL) return NULL;
  call->op = op;
  if (remaining != 0u) {
    call->len = remaining < IMAGE_CHUNK_SIZE ? remaining : IMAGE_CHUNK_SIZE;
    call->data =
        heap_caps_malloc(call->len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (call->data == NULL && call->len > IMAGE_FALLBACK_CHUNK_SIZE) {
      call->len = IMAGE_FALLBACK_CHUNK_SIZE;
      call->data =
          heap_caps_malloc(call->len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (call->data == NULL) {
      heap_caps_free(call);
      return NULL;
    }
  }
  return call;
}

static void image_call_free(image_call_t* call) {
  if (call != NULL) {
    heap_caps_free(call->data);
    heap_caps_free(call);
  }
}

static h2_pal_result_t get_info(void* user, uint32_t id,
                                h2_pal_image_view_t view,
                                h2_pal_image_info_t* out_info) {
  (void)user;
  if (out_info == NULL ||
      (view != H2_PAL_IMAGE_VIEW_STORED && view != H2_PAL_IMAGE_VIEW_NATIVE))
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_info, 0, sizeof(*out_info));
  image_call_t* call = image_call_alloc(IMAGE_INFO, 0u);
  if (call == NULL) return H2_PAL_ERR_NO_MEMORY;
  call->request.partition_id = id;
  call->view = view;
  h2_pal_result_t rc = image_submit(call);
  if (rc == H2_PAL_OK) *out_info = call->info;
  image_call_free(call);
  return rc;
}

static h2_pal_result_t read_image(void* user, uint32_t id,
                                  h2_pal_image_view_t view, uint64_t offset,
                                  void* data, size_t len) {
  (void)user;
  if ((data == NULL && len != 0u) || (uint64_t)len > UINT64_MAX - offset ||
      (view != H2_PAL_IMAGE_VIEW_STORED && view != H2_PAL_IMAGE_VIEW_NATIVE))
    return H2_PAL_ERR_INVALID_ARG;
  image_call_t* call = image_call_alloc(IMAGE_READ, len);
  if (call == NULL) return H2_PAL_ERR_NO_MEMORY;
  call->request.partition_id = id;
  const size_t capacity = call->len;
  h2_pal_result_t rc;
  size_t completed = 0u;
  do {
    call->offset = offset + completed;
    call->len = len - completed < capacity ? len - completed : capacity;
    rc = image_submit(call);
    if (rc != H2_PAL_OK) break;
    if (call->len != 0u)
      memcpy((uint8_t*)data + completed, call->data, call->len);
    completed += call->len;
  } while (completed < len);
  image_call_free(call);
  return rc;
}

static h2_pal_result_t begin_image(
    void* user, const h2_pal_image_write_request_t* request) {
  (void)user;
  if (request == NULL) return H2_PAL_ERR_INVALID_ARG;
  image_call_t* call = image_call_alloc(IMAGE_BEGIN, 0u);
  if (call == NULL) return H2_PAL_ERR_NO_MEMORY;
  call->request = *request;
  h2_pal_result_t rc = image_submit(call);
  image_call_free(call);
  return rc;
}

static h2_pal_result_t write_image(void* user, const void* data, size_t len) {
  (void)user;
  if (data == NULL && len != 0u) return H2_PAL_ERR_INVALID_ARG;
  image_call_t* call = image_call_alloc(IMAGE_WRITE, len);
  if (call == NULL) return H2_PAL_ERR_NO_MEMORY;
  const size_t capacity = call->len;
  h2_pal_result_t rc;
  size_t completed = 0u;
  do {
    call->len = len - completed < capacity ? len - completed : capacity;
    if (call->len != 0u)
      memcpy(call->data, (const uint8_t*)data + completed, call->len);
    rc = image_submit(call);
    if (rc != H2_PAL_OK) break;
    completed += call->len;
  } while (completed < len);
  image_call_free(call);
  return rc;
}

static h2_pal_result_t finish_image(void* user) {
  (void)user;
  image_call_t* call = image_call_alloc(IMAGE_FINISH, 0u);
  h2_pal_result_t rc = image_submit(call);
  image_call_free(call);
  return rc;
}

static h2_pal_result_t abort_image(void* user) {
  (void)user;
  image_call_t* call = image_call_alloc(IMAGE_ABORT, 0u);
  h2_pal_result_t rc = image_submit(call);
  image_call_free(call);
  return rc;
}

static const h2_pal_image_vtable_t s_vtable = {
    .get_info = get_info,
    .read = read_image,
    .begin = begin_image,
    .write = write_image,
    .finish = finish_image,
    .abort = abort_image,
};
static const h2_pal_image_api_t s_api = {.vtable = &s_vtable};

const h2_pal_image_api_t* h2_esp_platform_image_api(void) { return &s_api; }
