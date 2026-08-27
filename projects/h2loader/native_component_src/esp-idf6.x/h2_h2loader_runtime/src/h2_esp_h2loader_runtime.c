#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_h2loader_iostreamikcp_internal.h"

#include "h2loader_app.h"
#include "h2_loader_boot.h"
#include "h2_loader_command.h"
#include "h2_esp_platform_safe_call.h"
#include "h2_esp_platform_core.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

#include <stdio.h>
#include <string.h>

#define H2_LOADER_COREDUMP_PARTITION_ID 3u
#define H2_LOADER_STAGE_PATH "/dl/update.tar.zlib"
#define H2_LOADER_STAGE_PREV_PATH "/dl/update.tar.zlib.prev"
#define H2_LOADER_IMAGE_TASK_STACK_DEPTH 4096u
#define H2_LOADER_CONFIRM_TASK_STACK_DEPTH 4096u
#define H2_LOADER_IMAGE_INTERNAL_CHUNK_SIZE (16u * 1024u)
#define H2_LOADER_IMAGE_INTERNAL_FALLBACK_CHUNK_SIZE (4u * 1024u)

typedef struct h2_esp_h2loader_digest {
    psa_hash_operation_t sha;
    int active;
} h2_esp_h2loader_digest_t;

static h2_esp_h2loader_digest_t s_digest;
static int s_command_stop_requested;
static h2_esp_h2loader_command_transport_t s_command_transport;

typedef struct h2_esp_h2loader_confirm_call {
    int result;
} h2_esp_h2loader_confirm_call_t;

typedef struct h2_esp_h2loader_image_writer {
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    int active;
} h2_esp_h2loader_image_writer_t;

static h2_esp_h2loader_image_writer_t s_image_writer;

typedef enum h2_esp_h2loader_image_op {
    H2_ESP_H2LOADER_IMAGE_READ = 1,
    H2_ESP_H2LOADER_IMAGE_BEGIN,
    H2_ESP_H2LOADER_IMAGE_WRITE,
    H2_ESP_H2LOADER_IMAGE_FINISH,
    H2_ESP_H2LOADER_IMAGE_ABORT,
} h2_esp_h2loader_image_op_t;

typedef struct h2_esp_h2loader_image_call {
    h2_esp_h2loader_image_op_t op;
    uint32_t partition_id;
    uint64_t offset;
    uint64_t image_size;
    void *data;
    size_t len;
    int result;
} h2_esp_h2loader_image_call_t;

static StaticSemaphore_t s_image_mutex_storage;
static SemaphoreHandle_t s_image_mutex;
static portMUX_TYPE s_image_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct h2_esp_h2loader_context {
    h2_runtime_t *runtime;
    const h2_esp_h2loader_config_t *config;
} h2_esp_h2loader_context_t;

static int mount_file_point(void *user, const char *path) {
    const h2_esp_h2loader_context_t *context =
        (const h2_esp_h2loader_context_t *)user;
    return context != NULL && context->config != NULL &&
               context->config->mount_file_point != NULL
        ? context->config->mount_file_point(path)
        : H2_PAL_ERR_UNSUPPORTED;
}

static const esp_partition_t *image_partition(uint32_t partition_id) {
    esp_partition_subtype_t subtype;
    if (partition_id == 1u) {
        subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    } else if (partition_id == 2u) {
        subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    } else {
        return NULL;
    }
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, NULL);
}

static void image_writer_abort_internal(void) {
    if (s_image_writer.active) {
        (void)esp_ota_abort(s_image_writer.handle);
    }
    memset(&s_image_writer, 0, sizeof(s_image_writer));
}

static int image_partition_read_call(h2_esp_h2loader_image_call_t *call,
                                     const esp_partition_t *partition) {
    if (partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (call->len == 0u) {
        return H2_PAL_OK;
    }
    return esp_partition_read(partition, (size_t)call->offset, call->data,
                              call->len) == ESP_OK
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static int image_writer_begin_call(h2_esp_h2loader_image_call_t *call,
                                   const esp_partition_t *partition) {
    const esp_partition_t *running = esp_ota_get_running_partition();

    image_writer_abort_internal();
    if (partition == NULL || running == NULL ||
        partition->address == running->address || call->image_size == 0u ||
        call->image_size > partition->size || call->image_size > SIZE_MAX) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (esp_ota_begin(partition, (size_t)call->image_size,
                      &s_image_writer.handle) != ESP_OK) {
        return H2_PAL_ERR_IO;
    }
    s_image_writer.partition = partition;
    s_image_writer.active = 1;
    return H2_PAL_OK;
}

static int image_writer_write_call(h2_esp_h2loader_image_call_t *call) {
    if (!s_image_writer.active) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (call->len == 0u) {
        return H2_PAL_OK;
    }
    return esp_ota_write(s_image_writer.handle, call->data, call->len) == ESP_OK
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static int image_writer_finish_call(void) {
    int result;

    if (!s_image_writer.active) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    result = esp_ota_end(s_image_writer.handle) == ESP_OK ? H2_PAL_OK
                                                          : H2_PAL_ERR_IO;
    memset(&s_image_writer, 0, sizeof(s_image_writer));
    return result;
}

static void IRAM_ATTR image_safe_callback(void *context) {
    h2_esp_h2loader_image_call_t *call =
        (h2_esp_h2loader_image_call_t *)context;
    const esp_partition_t *partition = image_partition(call->partition_id);
    switch (call->op) {
        case H2_ESP_H2LOADER_IMAGE_READ:
            call->result = image_partition_read_call(call, partition);
            break;
        case H2_ESP_H2LOADER_IMAGE_BEGIN:
            call->result = image_writer_begin_call(call, partition);
            break;
        case H2_ESP_H2LOADER_IMAGE_WRITE:
            call->result = image_writer_write_call(call);
            break;
        case H2_ESP_H2LOADER_IMAGE_FINISH:
            call->result = image_writer_finish_call();
            break;
        case H2_ESP_H2LOADER_IMAGE_ABORT:
            image_writer_abort_internal();
            call->result = H2_PAL_OK;
            break;
        default:
            call->result = H2_PAL_ERR_INVALID_ARG;
            break;
    }
}

static SemaphoreHandle_t image_mutex(void) {
    portENTER_CRITICAL(&s_image_mutex_init_lock);
    if (s_image_mutex == NULL) {
        s_image_mutex = xSemaphoreCreateMutexStatic(&s_image_mutex_storage);
    }
    portEXIT_CRITICAL(&s_image_mutex_init_lock);
    return s_image_mutex;
}

static h2_esp_h2loader_image_call_t *image_call_alloc(
    h2_esp_h2loader_image_op_t op,
    size_t data_len) {
    h2_esp_h2loader_image_call_t *call =
        (h2_esp_h2loader_image_call_t *)heap_caps_calloc(
            1u,
            sizeof(*call),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (call == NULL) {
        return NULL;
    }
    call->op = op;
    if (data_len != 0u) {
        call->data = heap_caps_malloc(
            data_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (data_len != 0u && call->data == NULL) {
        heap_caps_free(call->data);
        heap_caps_free(call);
        return NULL;
    }
    call->len = data_len;
    return call;
}

static void image_call_free(h2_esp_h2loader_image_call_t *call) {
    if (call != NULL) {
        heap_caps_free(call->data);
        heap_caps_free(call);
    }
}

static h2_esp_h2loader_image_call_t *image_data_call_alloc(
    h2_esp_h2loader_image_op_t op,
    size_t remaining,
    size_t *out_len) {
    size_t chunk_len = remaining < H2_LOADER_IMAGE_INTERNAL_CHUNK_SIZE
        ? remaining
        : H2_LOADER_IMAGE_INTERNAL_CHUNK_SIZE;
    h2_esp_h2loader_image_call_t *call = image_call_alloc(op, chunk_len);
    if (call == NULL && chunk_len > H2_LOADER_IMAGE_INTERNAL_FALLBACK_CHUNK_SIZE) {
        chunk_len = remaining < H2_LOADER_IMAGE_INTERNAL_FALLBACK_CHUNK_SIZE
            ? remaining
            : H2_LOADER_IMAGE_INTERNAL_FALLBACK_CHUNK_SIZE;
        call = image_call_alloc(op, chunk_len);
    }
    if (call != NULL) {
        *out_len = chunk_len;
    }
    return call;
}

static int image_call_submit(h2_esp_h2loader_image_call_t *call) {
    SemaphoreHandle_t mutex;
    h2_pal_result_t rc;
    if (call == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    mutex = image_mutex();
    if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_TASK;
    }
    rc = h2_esp_platform_safe_call(
        image_safe_callback,
        call,
        sizeof(*call),
        H2_LOADER_IMAGE_TASK_STACK_DEPTH);
    (void)xSemaphoreGive(mutex);
    return rc == H2_PAL_OK ? call->result : rc;
}

static int image_capacity(void *user, uint32_t partition_id,
                          uint64_t *out_capacity) {
    const esp_partition_t *partition;
    (void)user;
    if (out_capacity == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    partition = image_partition(partition_id);
    if (partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_capacity = partition->size;
    return H2_PAL_OK;
}

static int image_read(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    void *data,
    size_t len) {
    const esp_partition_t *partition = image_partition(partition_id);
    size_t completed = 0u;
    (void)user;
    if (partition == NULL || (data == NULL && len != 0u) ||
        offset > partition->size || len > partition->size - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    while (completed < len) {
        size_t chunk_len = 0u;
        h2_esp_h2loader_image_call_t *call = image_data_call_alloc(
            H2_ESP_H2LOADER_IMAGE_READ, len - completed, &chunk_len);
        if (call == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        call->partition_id = partition_id;
        call->offset = offset + completed;
        const int rc = image_call_submit(call);
        if (rc == H2_PAL_OK) {
            memcpy((uint8_t *)data + completed, call->data, chunk_len);
        }
        image_call_free(call);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        completed += chunk_len;
    }
    return H2_PAL_OK;
}

static void image_writer_abort(void *user) {
    h2_esp_h2loader_image_call_t *call;
    (void)user;
    call = image_call_alloc(H2_ESP_H2LOADER_IMAGE_ABORT, 0u);
    if (call != NULL) {
        (void)image_call_submit(call);
        image_call_free(call);
    }
}

static int image_writer_begin(
    void *user,
    uint32_t partition_id,
    const h2_loader_image_identity_t *identity) {
    h2_esp_h2loader_image_call_t *call;
    const esp_partition_t *destination = image_partition(partition_id);
    int rc;
    (void)user;
    if (identity == NULL || destination == NULL ||
        identity->image_size == 0u ||
        identity->image_size > destination->size ||
        identity->image_size > SIZE_MAX) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    call = image_call_alloc(H2_ESP_H2LOADER_IMAGE_BEGIN, 0u);
    if (call == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    call->partition_id = partition_id;
    call->image_size = identity->image_size;
    rc = image_call_submit(call);
    image_call_free(call);
    return rc;
}

static int image_writer_write(void *user, const void *data, size_t len) {
    size_t completed = 0u;
    (void)user;
    if (data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    while (completed < len) {
        size_t chunk_len = 0u;
        h2_esp_h2loader_image_call_t *call = image_data_call_alloc(
            H2_ESP_H2LOADER_IMAGE_WRITE, len - completed, &chunk_len);
        if (call == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(call->data, (const uint8_t *)data + completed, chunk_len);
        const int rc = image_call_submit(call);
        image_call_free(call);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        completed += chunk_len;
    }
    return H2_PAL_OK;
}

static int image_writer_finish(
    void *user,
    const h2_loader_image_identity_t *identity) {
    h2_esp_h2loader_image_call_t *call;
    int rc;
    (void)user;
    (void)identity;
    call = image_call_alloc(H2_ESP_H2LOADER_IMAGE_FINISH, 0u);
    if (call == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    rc = image_call_submit(call);
    image_call_free(call);
    return rc;
}

static const h2_loader_image_reader_vtable_t s_image_reader_vtable = {
    .get_capacity = image_capacity,
    .read = image_read,
};

static const h2_loader_image_reader_api_t s_image_reader = {
    .vtable = &s_image_reader_vtable,
};

static const h2_loader_image_writer_vtable_t s_image_writer_vtable = {
    .get_capacity = image_capacity,
    .begin = image_writer_begin,
    .write = image_writer_write,
    .finish = image_writer_finish,
    .abort = image_writer_abort,
};

static const h2_loader_image_writer_api_t s_image_writer_api = {
    .user = &s_image_writer,
    .vtable = &s_image_writer_vtable,
};

static const char *event_name(h2_loader_startup_event_t event) {
    switch (event) {
    case H2_LOADER_STARTUP_EVENT_INSTALL_BEGIN:
        return "install_begin";
    case H2_LOADER_STARTUP_EVENT_INSTALL_SKIP_SAME_IDENTITY:
        return "install_skip_same_identity";
    case H2_LOADER_STARTUP_EVENT_BOOT_APP:
        return "boot_app";
    case H2_LOADER_STARTUP_EVENT_MAIN_FAILED:
        return "main_failed";
    case H2_LOADER_STARTUP_EVENT_INSTALL_FAILED:
        return "install_failed";
    default:
        return "unknown";
    }
}

static void on_event(void *user, h2_loader_startup_event_t event, int code) {
    h2_esp_h2loader_context_t *context = (h2_esp_h2loader_context_t *)user;
    printf("H2_LOADER_STARTUP_EVENT event=%s code=%d\n", event_name(event), code);
    fflush(stdout);
    if (context == NULL || context->config == NULL) {
        return;
    }
    if (event == H2_LOADER_STARTUP_EVENT_INSTALL_BEGIN &&
        context->config->show_installing != NULL) {
        context->config->show_installing(context->config->user);
    } else if (event == H2_LOADER_STARTUP_EVENT_BOOT_APP &&
               context->config->show_launching != NULL) {
        context->config->show_launching(context->config->user);
    } else if ((event == H2_LOADER_STARTUP_EVENT_MAIN_FAILED ||
                event == H2_LOADER_STARTUP_EVENT_INSTALL_FAILED) &&
               context->config->show_install_failed != NULL) {
        context->config->show_install_failed(context->config->user, code);
    }
}

static int digest_start(void *user) {
    h2_esp_h2loader_digest_t *digest = (h2_esp_h2loader_digest_t *)user;
    if (digest == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (digest->active) {
        (void)psa_hash_abort(&digest->sha);
    }
    digest->sha = psa_hash_operation_init();
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&digest->sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        (void)psa_hash_abort(&digest->sha);
        digest->active = 0;
        return H2_PAL_ERR_IO;
    }
    digest->active = 1;
    return H2_PAL_OK;
}

static int digest_update(void *user, const uint8_t *data, size_t len) {
    h2_esp_h2loader_digest_t *digest = (h2_esp_h2loader_digest_t *)user;
    if (digest == NULL || !digest->active || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return psa_hash_update(&digest->sha, data, len) == PSA_SUCCESS
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static int digest_finish(void *user, uint8_t out_digest[32]) {
    h2_esp_h2loader_digest_t *digest = (h2_esp_h2loader_digest_t *)user;
    psa_status_t status;
    size_t digest_len = 0u;
    if (digest == NULL || !digest->active || out_digest == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    status = psa_hash_finish(&digest->sha, out_digest, 32u, &digest_len);
    (void)psa_hash_abort(&digest->sha);
    digest->active = 0;
    return status == PSA_SUCCESS && digest_len == 32u
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static void digest_abort(void *user) {
    h2_esp_h2loader_digest_t *digest = (h2_esp_h2loader_digest_t *)user;
    if (digest != NULL && digest->active) {
        (void)psa_hash_abort(&digest->sha);
        digest->active = 0;
    }
}

static uint64_t now_ms(void *user) {
    (void)user;
    return (uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS;
}

static void sleep_ms(void *user, uint32_t delay_ms) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static void IRAM_ATTR confirm_pending_h2loader_boot_safe(void *context) {
    h2_esp_h2loader_confirm_call_t *call =
        (h2_esp_h2loader_confirm_call_t *)context;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err;

    if (running == NULL) {
        call->result = H2_PAL_ERR_NOT_FOUND;
        return;
    }
    err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_ERR_NOT_FOUND) {
        call->result = H2_PAL_OK;
        return;
    }
    if (err != ESP_OK) {
        call->result = H2_PAL_ERR_IO;
        return;
    }
    if (state == ESP_OTA_IMG_UNDEFINED || state == ESP_OTA_IMG_VALID) {
        call->result = H2_PAL_OK;
        return;
    }
    if (state != ESP_OTA_IMG_NEW && state != ESP_OTA_IMG_PENDING_VERIFY) {
        call->result = H2_PAL_OK;
        return;
    }
    call->result = esp_ota_mark_app_valid_cancel_rollback() == ESP_OK
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static int confirm_pending_h2loader_boot(void) {
    h2_esp_h2loader_confirm_call_t call = {0};
    h2_pal_result_t rc = h2_esp_platform_safe_call(
        confirm_pending_h2loader_boot_safe,
        &call,
        sizeof(call),
        H2_LOADER_CONFIRM_TASK_STACK_DEPTH);
    return rc == H2_PAL_OK ? call.result : rc;
}

int h2_esp_h2loader_app_confirm(h2_runtime_t *runtime) {
    if (runtime == NULL || runtime->pref == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = confirm_pending_h2loader_boot();
    if (rc == H2_PAL_OK) {
        rc = h2_loader_mark_app_confirmed(runtime->pref);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_platform_pref_finalize_migration();
    }
    return rc;
}

static int confirm_active_image(void *user) {
    (void)user;
    return confirm_pending_h2loader_boot();
}

static h2_loader_memory_region_stats_t memory_region_stats(uint32_t caps) {
    return (h2_loader_memory_region_stats_t){
        .total_bytes = heap_caps_get_total_size(caps),
        .free_bytes = heap_caps_get_free_size(caps),
        .minimum_free_bytes = heap_caps_get_minimum_free_size(caps),
        .largest_free_block_bytes = heap_caps_get_largest_free_block(caps),
    };
}

h2_pal_result_t h2_esp_h2loader_memory_stats_read(
    void *user,
    h2_loader_memory_stats_t *out_stats) {
    (void)user;
    if (out_stats == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_stats = (h2_loader_memory_stats_t){
        .internal = memory_region_stats(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .iram = memory_region_stats(MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT),
        .psram = memory_region_stats(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    return H2_PAL_OK;
}

static int runtime_fs_clear_adapter(void *user, const char *path) {
    return h2_pal_fs_clear((const h2_pal_fs_api_t *)user, path);
}

static int prepare_loader(void *user, h2_loader_t *loader) {
    h2_esp_h2loader_context_t *context = (h2_esp_h2loader_context_t *)user;
    h2_runtime_t *runtime = context->runtime;
    (void)loader;
    return h2_loader_package_recover_publish(
        runtime->fs,
        runtime->pref,
        H2_LOADER_STAGE_PATH,
        H2_LOADER_STAGE_PREV_PATH);
}

static int serve_loader(
    void *user,
    h2_loader_t *loader,
    h2_loader_command_t *command,
    h2_loader_startup_action_t action) {
    h2_loader_command_config_t command_config = command->config;
    int rc;

    (void)user;
    (void)loader;
    printf("H2_LOADER_READY target=esp status=ready action=%d\n", (int)action);
    fflush(stdout);
    __atomic_store_n(&s_command_stop_requested, 0, __ATOMIC_RELEASE);
    while (__atomic_load_n(&s_command_stop_requested, __ATOMIC_ACQUIRE) == 0) {
        if (s_command_transport.pending_conv != 0u) {
            rc = h2_loader_command_init(command, &command_config);
            if (rc == H2_PAL_OK) {
                rc = h2_esp_h2loader_command_transport_activate_pending(&s_command_transport);
            }
            if (rc != H2_PAL_OK) {
                printf("H2_LOADER_ERROR reason=session_activate_failed code=%d\n", rc);
                fflush(stdout);
            }
            continue;
        }
        if (!h2_esp_h2loader_command_transport_has_session(&s_command_transport)) {
            rc = h2_esp_h2loader_command_transport_poll_session(&s_command_transport, 50u);
            if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_WOULD_BLOCK) {
                printf("H2_LOADER_ERROR reason=session_poll_failed code=%d\n", rc);
                fflush(stdout);
            }
            continue;
        }
        rc = h2_loader_command_poll(command, 50u);
        if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
            continue;
        }
        if (rc == H2_PAL_ERR_CLOSED &&
            h2_esp_h2loader_command_transport_replacement_pending(&s_command_transport)) {
            continue;
        }
        if (rc != H2_PAL_OK) {
            printf("H2_LOADER_ERROR reason=command_failed code=%d\n", rc);
        }
        fflush(stdout);
    }
    return H2_PAL_OK;
}

static int stop_serve_loader(void *user) {
    (void)user;
    __atomic_store_n(&s_command_stop_requested, 1, __ATOMIC_RELEASE);
    return H2_PAL_OK;
}

void h2_esp_h2loader_run_with_command_service_config(
    h2_runtime_t *runtime,
    const h2_esp_h2loader_config_t *runtime_loader_config,
    const h2loader_app_command_service_api_t *command_service) {
    h2loader_app_config_t config;
    int rc;
    const char *board;
    h2_esp_h2loader_context_t context;
    h2_pal_firmware_info_t firmware_info;

    if (runtime == NULL || runtime_loader_config == NULL ||
        runtime_loader_config->board == NULL || runtime->fs == NULL ||
        runtime->disk == NULL ||
        runtime->pref == NULL || runtime->power == NULL ||
        runtime->mem == NULL || runtime->http == NULL ||
        runtime->wifi_sta == NULL) {
        printf("H2_LOADER_READY target=esp status=runtime_config_fail\n");
        return;
    }
    board = runtime_loader_config->board;
    rc = h2_pal_firmware_info_get_current(
        runtime->firmware_info,
        &firmware_info);
    if (rc != H2_PAL_OK) {
        printf(
            "H2_LOADER_READY target=esp status=firmware_info_fail code=%d\n",
            rc);
        return;
    }
    context = (h2_esp_h2loader_context_t){
        .runtime = runtime,
        .config = runtime_loader_config,
    };
    if (h2_esp_h2loader_console_init() != H2_PAL_OK ||
        h2_esp_h2loader_command_transport_init(&s_command_transport, runtime->mem) != H2_PAL_OK) {
        printf("H2_LOADER_READY target=esp status=serial_fail\n");
        return;
    }
    memset(&config, 0, sizeof(config));
    config.loader.package.fs = runtime->fs;
    config.loader.package.disk = runtime->disk;
    config.loader.package.digest.user = &s_digest;
    config.loader.package.digest.start = digest_start;
    config.loader.package.digest.update = digest_update;
    config.loader.package.digest.finish = digest_finish;
    config.loader.package.digest.abort = digest_abort;
    config.loader.package.clear_data = runtime_fs_clear_adapter;
    config.loader.package.clear_data_user = (void *)runtime->fs;
    config.loader.package.image_reader = &s_image_reader;
    config.loader.package.image_writer = &s_image_writer_api;
    config.loader.package.progress_user = runtime_loader_config->user;
    config.loader.package.progress = runtime_loader_config->install_progress;
    config.loader.board = board;
    config.loader.target = CONFIG_IDF_TARGET;
    config.loader.h2loader_partition_id = 1u;
    config.loader.app_partition_id = 2u;
    config.loader.mfg_required_total = runtime_loader_config->mfg_required_total;
    config.loader.hardware_capabilities =
        H2_LOADER_CAPABILITY_UART |
        H2_LOADER_CAPABILITY_WIFI |
        H2_LOADER_CAPABILITY_BLE;
    config.loader.active_identity.format = 1u;
    config.loader.active_identity.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
    (void)snprintf(
        config.loader.active_identity.board,
        sizeof(config.loader.active_identity.board),
        "%s",
        board);
    (void)snprintf(
        config.loader.active_identity.target,
        sizeof(config.loader.active_identity.target),
        "%s",
        CONFIG_IDF_TARGET);
    (void)snprintf(
        config.loader.active_identity.version,
        sizeof(config.loader.active_identity.version),
        "%s",
        firmware_info.version);
    config.loader.confirm_active_image = confirm_active_image;
    if (runtime_loader_config->mount_file_point != NULL) {
        config.loader.mount_user = &context;
        config.loader.mount_file_point = mount_file_point;
    }
    config.loader.event_user = &context;
    config.loader.on_event = on_event;
    config.loader.disruptive_user = runtime_loader_config->user;
    config.loader.before_disruptive = runtime_loader_config->before_disruptive;
    config.command.digest = config.loader.package.digest;
    config.command.memory_stats.read = h2_esp_h2loader_memory_stats_read;
    config.command.now_ms = now_ms;
    config.command.sleep_ms = sleep_ms;
    config.command.io = h2_esp_h2loader_command_transport_io(&s_command_transport);
    config.command.coredump_partition_id = H2_LOADER_COREDUMP_PARTITION_ID;
    config.user = &context;
    config.prepare = prepare_loader;
    config.before_startup_user = runtime_loader_config->user;
    config.before_startup = runtime_loader_config->run_mfg;
    config.rearm_before_startup = runtime_loader_config->rearm_mfg;
    config.stop_serve = stop_serve_loader;
    config.serve = serve_loader;
    rc = h2loader_app_run_with_command_service(
        runtime, &config, command_service);
    h2_esp_h2loader_command_transport_deinit(&s_command_transport);
    printf("H2_LOADER_READY target=esp status=app_exit code=%d\n", rc);
}

void h2_esp_h2loader_run_with_config(
    h2_runtime_t *runtime,
    const h2_esp_h2loader_config_t *runtime_loader_config) {
    h2_esp_h2loader_run_with_command_service_config(
        runtime, runtime_loader_config, NULL);
}

void h2_esp_h2loader_run(
    h2_runtime_t *runtime,
    const char *board,
    h2_esp_h2loader_mount_fn mount_file_point) {
    const h2_esp_h2loader_config_t config = {
        .board = board,
        .mount_file_point = mount_file_point,
    };
    h2_esp_h2loader_run_with_config(runtime, &config);
}
