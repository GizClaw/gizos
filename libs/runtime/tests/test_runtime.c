#include "h2_runtime_internal.h"
#include "h2_runtime_test.h"

#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/hal/h2_pal_gpio_irq.h"
#include "h2/pal/hal/h2_pal_modem.h"
#include "h2/pal/hal/h2_pal_wifi.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

const h2_pal_buzzer_api_t *h2_runtime_test_buzzer_api(void);
void h2_runtime_test_buzzer_binding(const h2_runtime_t *runtime);

struct h2_pal_system_event_subscription {
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
    int active;
};

struct h2_pal_queue {
    size_t item_size;
    size_t item_count;
    size_t count;
    size_t head;
    size_t tail;
    uint8_t *items;
    const h2_pal_mem_api_t *mem;
    int closed;
};

struct h2_pal_task {
    h2_pal_task_entry_t entry;
    void *ctx;
};

struct h2_pal_mutex {
    int locked;
    const h2_pal_mem_api_t *mem;
};

struct h2_pal_cond {
    const h2_pal_mem_api_t *mem;
};

typedef struct test_allocator {
    size_t alloc_calls;
    size_t free_calls;
    size_t fail_on_call;
    size_t live_allocations;
    int fill_allocations_nonzero;
} test_allocator_t;

typedef struct test_time {
    uint64_t now_ms;
    uint32_t sleep_calls;
    h2_pal_result_t sleep_rc;
    h2_runtime_t *stop_after_sleep_runtime;
} test_time_t;

typedef struct test_queue_state {
    h2_pal_result_t full_rc;
    h2_pal_result_t send_rc;
    h2_pal_result_t reset_rc;
} test_queue_state_t;

typedef struct test_periph_set {
    h2_pal_periph_info_t infos[16];
    size_t count;
    size_t get_calls;
    size_t list_calls;
} test_periph_set_t;

typedef struct test_button {
    h2_pal_button_state_t single_state;
    h2_pal_periph_id_t radio_pressed_id;
    h2_pal_result_t group_rc;
    size_t single_reads;
    size_t group_reads;
} test_button_t;

typedef struct test_nfc {
    h2_pal_nfc_scan_t scan;
    h2_pal_result_t scan_rc;
    h2_pal_result_t read_rc;
    uint8_t read_bytes[4];
    uint8_t last_expected_uid[H2_PAL_NFC_UID_MAX_LEN];
    uint8_t last_expected_uid_len;
    size_t scan_calls;
} test_nfc_t;

typedef struct test_nfc_card_emulation {
    size_t capability_calls;
    h2_pal_periph_id_t last_periph_id;
} test_nfc_card_emulation_t;

typedef struct test_imu {
    h2_pal_imu_reading_t reading;
    h2_pal_result_t rc;
} test_imu_t;

typedef struct test_input {
    h2_pal_battery_reading_t battery;
    h2_pal_temperature_reading_t temperature;
    h2_pal_result_t battery_rc;
    h2_pal_result_t temperature_rc;
    size_t battery_reads;
    size_t temperature_reads;
} test_input_t;

typedef struct test_task {
    h2_pal_task_t *current;
    h2_pal_task_t *handles[4];
    size_t handle_count;
    h2_pal_result_t start_rc;
    h2_pal_result_t join_rc;
    size_t starts;
    size_t joins;
    h2_pal_task_options_t options;
} test_task_t;

typedef struct test_sync {
    size_t creates;
    size_t destroys;
    size_t locks;
    size_t unlocks;
    h2_pal_result_t cond_create_rc;
    h2_pal_result_t cond_wait_rc;
    h2_pal_result_t cond_broadcast_rc;
} test_sync_t;

typedef struct test_system_event {
    h2_pal_system_event_subscription_t subscriptions[H2_RUNTIME_SYSTEM_EVENT_SUBSCRIPTION_MAX];
    size_t subscribe_calls;
    size_t unsubscribe_calls;
    size_t init_calls;
    size_t deinit_calls;
    size_t fail_subscribe_after;
} test_system_event_t;

typedef struct test_runtime_env {
    test_allocator_t allocator_state;
    h2_pal_mem_api_t mem;
    test_time_t time_state;
    h2_pal_time_api_t time;
    test_queue_state_t queue_state;
    h2_pal_queue_api_t queue;
    test_periph_set_t periphs;
    h2_pal_periph_api_t periph;
    h2_runtime_component_mapper_t component_mapper;
    test_button_t button_state;
    h2_pal_button_api_t button;
    test_nfc_t nfc_state;
    h2_pal_nfc_api_t nfc;
    test_nfc_card_emulation_t nfc_card_emulation_state;
    h2_pal_nfc_card_emulation_api_t nfc_card_emulation;
    test_imu_t imu_state;
    h2_pal_imu_api_t imu;
    test_input_t input_state;
    h2_pal_input_api_t input;
    test_task_t task_state;
    h2_pal_task_api_t task;
    test_sync_t sync_state;
    h2_pal_sync_api_t sync;
    test_system_event_t system_event_state;
    h2_pal_system_event_api_t system_event;
} test_runtime_env_t;

static h2_pal_result_t test_input_read_battery(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_battery_reading_t *out_reading) {
    test_input_t *state = user;
    state->battery_reads++;
    *out_reading = state->battery;
    out_reading->id = id;
    return state->battery_rc;
}

static h2_pal_result_t test_input_read_temperature(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_temperature_reading_t *out_reading) {
    test_input_t *state = user;
    state->temperature_reads++;
    *out_reading = state->temperature;
    out_reading->id = id;
    return state->temperature_rc;
}

static h2_pal_result_t test_nfc_card_emulation_get_capabilities(
    void *user,
    h2_pal_periph_id_t periph_id,
    h2_pal_nfc_card_emulation_capabilities_t *out_capabilities) {
    test_nfc_card_emulation_t *state = user;
    state->capability_calls++;
    state->last_periph_id = periph_id;
    *out_capabilities = (h2_pal_nfc_card_emulation_capabilities_t){
        .technology_mask =
            H2_PAL_NFC_CARD_EMULATION_TECHNOLOGY_ISO14443A,
        .exchange_mode_mask = H2_PAL_NFC_CARD_EMULATION_MODE_RAW_FRAME,
        .min_uid_len = 7u,
        .max_uid_len = 7u,
    };
    return H2_PAL_OK;
}

static h2_runtime_component_t test_component_from_periph_type(
    h2_pal_periph_type_t type) {
    switch (type) {
    case H2_PAL_PERIPH_TYPE_SINGLE_BUTTON:
    case H2_PAL_PERIPH_TYPE_RADIO_BUTTON:
        return H2_RUNTIME_COMPONENT_BUTTON;
    case H2_PAL_PERIPH_TYPE_NFC_READER:
        return H2_RUNTIME_COMPONENT_NFC_READER;
    case H2_PAL_PERIPH_TYPE_IMU:
        return H2_RUNTIME_COMPONENT_IMU;
    case H2_PAL_PERIPH_TYPE_BATTERY:
        return H2_RUNTIME_COMPONENT_BATTERY;
    case H2_PAL_PERIPH_TYPE_PWM_SWITCH:
        return H2_RUNTIME_COMPONENT_PWM_SWITCH;
    case H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR:
        return H2_RUNTIME_COMPONENT_TEMPERATURE_SENSOR;
    case H2_PAL_PERIPH_TYPE_BUZZER:
        return H2_RUNTIME_COMPONENT_BUZZER;
    case H2_PAL_PERIPH_TYPE_GPIO_IRQ:
        return H2_RUNTIME_COMPONENT_SYSTEM_GPIO_IRQ;
    default:
        return H2_RUNTIME_COMPONENT_NONE;
    }
}

static h2_pal_result_t test_mapper_list(
    void *user,
    h2_runtime_component_t component_filter,
    h2_runtime_component_mapping_cb_t cb,
    void *cb_user) {
    test_runtime_env_t *env = (test_runtime_env_t *)user;
    test_periph_set_t *periphs;
    if (env == NULL || cb == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    periphs = &env->periphs;
    for (size_t i = 0u; i < periphs->count; ++i) {
        if (test_component_from_periph_type(periphs->infos[i].type) != component_filter) {
            continue;
        }
        const h2_runtime_component_mapping_entry_t entry = {
            .component_id = (h2_runtime_component_id_t)(i + 1u),
            .periph_id = periphs->infos[i].id,
        };
        h2_pal_result_t rc = cb(cb_user, &entry);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static void *test_alloc(void *user, size_t len) {
    test_allocator_t *state = (test_allocator_t *)user;
    state->alloc_calls += 1u;
    if (state->fail_on_call == state->alloc_calls) {
        return NULL;
    }
    void *ptr = malloc(len);
    if (ptr != NULL) {
        memset(ptr, state->fill_allocations_nonzero != 0 ? 0xa5 : 0, len);
        state->live_allocations += 1u;
    }
    return ptr;
}

static void test_free(void *user, void *ptr) {
    test_allocator_t *state = (test_allocator_t *)user;
    state->free_calls += 1u;
    assert(ptr != NULL);
    assert(state->live_allocations != 0u);
    state->live_allocations -= 1u;
    free(ptr);
}

static h2_pal_result_t test_time_now(void *user, uint64_t *out_ms) {
    test_time_t *time = (test_time_t *)user;
    *out_ms = time->now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t test_sleep(void *user, uint32_t ms) {
    test_time_t *time = (test_time_t *)user;
    time->sleep_calls += 1u;
    time->now_ms += ms;
    if (time->stop_after_sleep_runtime != NULL) {
        atomic_store(
            &time->stop_after_sleep_runtime->private_state->input_stop_requested,
            1);
    }
    return time->sleep_rc;
}

static h2_pal_result_t test_sync_create_mutex(
    void *user,
    const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
    test_sync_t *sync = (test_sync_t *)user;
    h2_pal_mutex_t *mutex =
        (h2_pal_mutex_t *)h2_pal_mem_alloc(config->allocator, sizeof(*mutex));
    if (mutex == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(mutex, 0, sizeof(*mutex));
    mutex->mem = config->allocator;
    sync->creates += 1u;
    *out_mutex = mutex;
    return H2_PAL_OK;
}

static h2_pal_result_t test_sync_destroy_mutex(void *user, h2_pal_mutex_t *mutex) {
    test_sync_t *sync = (test_sync_t *)user;
    sync->destroys += 1u;
    h2_pal_mem_free(mutex->mem, mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t test_sync_lock_mutex(void *user, h2_pal_mutex_t *mutex) {
    test_sync_t *sync = (test_sync_t *)user;
    sync->locks += 1u;
    mutex->locked = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t test_sync_unlock_mutex(void *user, h2_pal_mutex_t *mutex) {
    test_sync_t *sync = (test_sync_t *)user;
    sync->unlocks += 1u;
    mutex->locked = 0;
    return H2_PAL_OK;
}

static h2_pal_result_t test_sync_create_cond(
    void *user,
    const h2_pal_cond_config_t *config,
    h2_pal_cond_t **out_cond) {
    test_sync_t *sync = (test_sync_t *)user;
    if (sync->cond_create_rc != H2_PAL_OK) {
        return sync->cond_create_rc;
    }
    h2_pal_cond_t *cond =
        (h2_pal_cond_t *)h2_pal_mem_alloc(config->allocator, sizeof(*cond));
    if (cond == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    cond->mem = config->allocator;
    sync->creates += 1u;
    *out_cond = cond;
    return H2_PAL_OK;
}

static h2_pal_result_t test_sync_destroy_cond(
    void *user,
    h2_pal_cond_t *cond) {
    test_sync_t *sync = (test_sync_t *)user;
    sync->destroys += 1u;
    h2_pal_mem_free(cond->mem, cond);
    return H2_PAL_OK;
}

static h2_pal_result_t test_sync_wait_cond(
    void *user,
    h2_pal_cond_t *cond,
    h2_pal_mutex_t *mutex,
    uint32_t timeout_ms) {
    test_sync_t *sync = (test_sync_t *)user;
    (void)cond;
    (void)mutex;
    (void)timeout_ms;
    return sync->cond_wait_rc != H2_PAL_OK
               ? sync->cond_wait_rc
               : H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t test_sync_broadcast_cond(
    void *user,
    h2_pal_cond_t *cond) {
    test_sync_t *sync = (test_sync_t *)user;
    (void)cond;
    return sync->cond_broadcast_rc;
}

static int test_queue_create(
    void *user,
    const h2_pal_queue_config_t *config,
    h2_pal_queue_t **out_queue) {
    test_queue_state_t *state = (test_queue_state_t *)user;
    h2_pal_queue_t *queue =
        (h2_pal_queue_t *)h2_pal_mem_alloc(config->allocator, sizeof(*queue));
    if (queue == NULL) return H2_PAL_ERR_NO_MEMORY;
    queue->items = (uint8_t *)h2_pal_mem_alloc(
        config->allocator,
        config->item_size * config->item_count);
    if (queue->items == NULL) {
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    queue->item_size = config->item_size;
    queue->item_count = config->item_count;
    queue->mem = config->allocator;
    if (state != NULL && state->full_rc == H2_PAL_OK) {
        state->full_rc = H2_PAL_ERR_FULL;
    }
    *out_queue = queue;
    return H2_PAL_OK;
}

static void test_queue_destroy(void *user, h2_pal_queue_t *queue) {
    (void)user;
    const h2_pal_mem_api_t *mem = queue->mem;
    h2_pal_mem_free(mem, queue->items);
    h2_pal_mem_free(mem, queue);
}

static int test_queue_send(
    void *user,
    h2_pal_queue_t *queue,
    const void *item,
    uint32_t timeout_ms) {
    test_queue_state_t *state = (test_queue_state_t *)user;
    (void)timeout_ms;
    if (queue->closed != 0) {
        return H2_PAL_ERR_CLOSED;
    }
    if (state != NULL && state->send_rc != H2_PAL_OK) {
        return state->send_rc;
    }
    if (queue->count == queue->item_count) {
        return state != NULL && state->full_rc != H2_PAL_OK ? state->full_rc : H2_PAL_ERR_FULL;
    }
    memcpy(queue->items + queue->tail * queue->item_size, item, queue->item_size);
    queue->tail = (queue->tail + 1u) % queue->item_count;
    queue->count += 1u;
    return H2_PAL_OK;
}

static int test_queue_recv(
    void *user,
    h2_pal_queue_t *queue,
    void *out_item,
    uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    if (queue->count == 0u) {
        return queue->closed != 0 ? H2_PAL_ERR_CLOSED
                                  : H2_PAL_ERR_WOULD_BLOCK;
    }
    memcpy(out_item, queue->items + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1u) % queue->item_count;
    queue->count -= 1u;
    return H2_PAL_OK;
}

static int test_queue_reset(void *user, h2_pal_queue_t *queue) {
    test_queue_state_t *state = (test_queue_state_t *)user;
    if (queue == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (state != NULL && state->reset_rc != H2_PAL_OK) {
        return state->reset_rc;
    }
    queue->count = 0u;
    queue->head = 0u;
    queue->tail = 0u;
    queue->closed = 0;
    return H2_PAL_OK;
}

static int test_queue_close(void *user, h2_pal_queue_t *queue) {
    (void)user;
    if (queue == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    queue->closed = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t test_periph_get(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_periph_info_t *out_info) {
    test_periph_set_t *set = (test_periph_set_t *)user;
    set->get_calls += 1u;
    for (size_t i = 0u; i < set->count; ++i) {
        if (set->infos[i].id == id) {
            *out_info = set->infos[i];
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t test_periph_list(
    void *user,
    h2_pal_periph_type_t type_filter,
    h2_pal_periph_cb_t cb,
    void *cb_user) {
    test_periph_set_t *set = (test_periph_set_t *)user;
    set->list_calls += 1u;
    for (size_t i = 0u; i < set->count; ++i) {
        if (type_filter == H2_PAL_PERIPH_TYPE_ANY || set->infos[i].type == type_filter) {
            h2_pal_result_t rc = cb(cb_user, &set->infos[i]);
            if (rc != H2_PAL_OK) return rc;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t test_read_single_button(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_single_button_reading_t *out_reading) {
    test_button_t *button = (test_button_t *)user;
    button->single_reads += 1u;
    *out_reading = (h2_pal_single_button_reading_t){
        .id = id,
        .state = button->single_state,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t test_read_radio_group(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_radio_button_group_reading_t *out_reading) {
    test_button_t *button = (test_button_t *)user;
    button->group_reads += 1u;
    if (button->group_rc != H2_PAL_OK) return button->group_rc;
    *out_reading = (h2_pal_radio_button_group_reading_t){
        .id = id,
        .pressed_button_id = button->radio_pressed_id,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t test_nfc_scan(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_nfc_scan_t *out_scan) {
    test_nfc_t *nfc = (test_nfc_t *)user;
    nfc->scan_calls += 1u;
    if (nfc->scan_rc != H2_PAL_OK) return nfc->scan_rc;
    *out_scan = nfc->scan;
    out_scan->id = id;
    return H2_PAL_OK;
}

static h2_pal_result_t test_nfc_read(
    void *user,
    h2_pal_periph_id_t id,
    const uint8_t *expected_uid,
    uint8_t expected_uid_len,
    h2_pal_nfc_data_type_t requested_type,
    const h2_pal_mem_api_t *mem,
    h2_pal_nfc_data_read_t *out_data) {
    test_nfc_t *nfc = (test_nfc_t *)user;
    (void)id;
    if (nfc->read_rc != H2_PAL_OK) return nfc->read_rc;
    nfc->last_expected_uid_len = expected_uid_len;
    memcpy(nfc->last_expected_uid, expected_uid, expected_uid_len);
    uint8_t *bytes = (uint8_t *)h2_pal_mem_alloc(mem, sizeof(nfc->read_bytes));
    if (bytes == NULL) return H2_PAL_ERR_NO_MEMORY;
    memcpy(bytes, nfc->read_bytes, sizeof(nfc->read_bytes));
    *out_data = (h2_pal_nfc_data_read_t){
        .id = id,
        .tag_type = nfc->scan.tag_type,
        .uid_len = nfc->scan.uid_len,
        .type = requested_type,
        .bytes = bytes,
        .len = sizeof(nfc->read_bytes),
    };
    memcpy(out_data->uid, nfc->scan.uid, sizeof(out_data->uid));
    return H2_PAL_OK;
}

static h2_pal_result_t test_imu_read(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_imu_reading_t *out_reading) {
    test_imu_t *imu = (test_imu_t *)user;
    if (imu->rc != H2_PAL_OK) return imu->rc;
    *out_reading = imu->reading;
    out_reading->id = id;
    return H2_PAL_OK;
}

static int test_task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task) {
    test_task_t *task = (test_task_t *)user;
    (void)options;
    if (task->start_rc != H2_PAL_OK) {
        return task->start_rc;
    }
    h2_pal_task_t *handle = (h2_pal_task_t *)calloc(1u, sizeof(*handle));
    if (handle == NULL) return H2_PAL_ERR_NO_MEMORY;
    handle->entry = entry;
    handle->ctx = ctx;
    task->options = options != NULL ? *options : (h2_pal_task_options_t){0};
    if (task->handle_count >= sizeof(task->handles) / sizeof(task->handles[0])) {
        free(handle);
        return H2_PAL_ERR_NO_SPACE;
    }
    task->handles[task->handle_count++] = handle;
    if (task->current == NULL) {
        task->current = handle;
    }
    task->starts += 1u;
    *out_task = handle;
    return H2_PAL_OK;
}

static int test_task_join(void *user, h2_pal_task_t *handle) {
    test_task_t *task = (test_task_t *)user;
    if (handle == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (task->join_rc != H2_PAL_OK) return task->join_rc;
    size_t index = 0u;
    while (index < task->handle_count && task->handles[index] != handle) {
        ++index;
    }
    if (index == task->handle_count) return H2_PAL_ERR_INVALID_ARG;
    for (size_t next = index + 1u; next < task->handle_count; ++next) {
        task->handles[next - 1u] = task->handles[next];
    }
    task->handle_count -= 1u;
    task->joins += 1u;
    free(handle);
    if (task->current == handle) {
        task->current = task->handle_count > 0u ? task->handles[0] : NULL;
    }
    return H2_PAL_OK;
}

static int test_system_event_init(void *user) {
    test_system_event_t *events = (test_system_event_t *)user;
    events->init_calls += 1u;
    return H2_PAL_OK;
}

static void test_system_event_deinit(void *user) {
    test_system_event_t *events = (test_system_event_t *)user;
    events->deinit_calls += 1u;
}

static int test_system_event_subscribe(
    void *user,
    h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler,
    void *handler_user,
    h2_pal_system_event_subscription_t **out_subscription) {
    test_system_event_t *events = (test_system_event_t *)user;
    if (events->fail_subscribe_after != 0u &&
        events->subscribe_calls >= events->fail_subscribe_after) {
        return H2_PAL_ERR_IO;
    }
    if (events->subscribe_calls >= H2_RUNTIME_SYSTEM_EVENT_SUBSCRIPTION_MAX) {
        return H2_PAL_ERR_NO_SPACE;
    }
    h2_pal_system_event_subscription_t *subscription =
        &events->subscriptions[events->subscribe_calls++];
    *subscription = (h2_pal_system_event_subscription_t){
        .type = type,
        .handler = handler,
        .handler_user = handler_user,
        .active = 1,
    };
    *out_subscription = subscription;
    return H2_PAL_OK;
}

static void test_system_event_unsubscribe(
    void *user,
    h2_pal_system_event_subscription_t *subscription) {
    test_system_event_t *events = (test_system_event_t *)user;
    if (subscription != NULL && subscription->active != 0) {
        subscription->active = 0;
        events->unsubscribe_calls += 1u;
    }
}

static int test_system_event_dispatch(
    test_runtime_env_t *env,
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payload_size) {
    h2_pal_system_event_t event = {
        .type = type,
        .payload = payload,
        .payload_size = payload_size,
    };
    for (size_t i = 0u; i < env->system_event_state.subscribe_calls; ++i) {
        h2_pal_system_event_subscription_t *subscription =
            &env->system_event_state.subscriptions[i];
        if (subscription->active != 0 && subscription->type == type) {
            return subscription->handler(subscription->handler_user, &event);
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static void test_env_init(test_runtime_env_t *env) {
    memset(env, 0, sizeof(*env));
    static const h2_pal_mem_vtable_t allocator_vtable = {
        .alloc = test_alloc,
        .free = test_free,
    };
    env->mem = (h2_pal_mem_api_t){
        .user = &env->allocator_state,
        .vtable = &allocator_vtable,
    };
    static const h2_pal_time_vtable_t time_vtable = {
        .get_monotonic_ms = test_time_now,
        .sleep_ms = test_sleep,
    };
    env->time = (h2_pal_time_api_t){ .user = &env->time_state, .vtable = &time_vtable };
    env->queue_state.full_rc = H2_PAL_ERR_FULL;
    static const h2_pal_queue_vtable_t queue_vtable = {
        .create = test_queue_create,
        .destroy = test_queue_destroy,
        .send = test_queue_send,
        .recv = test_queue_recv,
        .reset = test_queue_reset,
        .close = test_queue_close,
    };
    env->queue = (h2_pal_queue_api_t){
        .user = &env->queue_state,
        .vtable = &queue_vtable,
    };
    static const h2_pal_periph_vtable_t periph_vtable = {
        .list = test_periph_list,
        .get = test_periph_get,
    };
    env->periph = (h2_pal_periph_api_t){ .user = &env->periphs, .vtable = &periph_vtable };
    static const h2_runtime_component_mapper_vtable_t component_mapper_vtable = {
        .list = test_mapper_list,
    };
    env->component_mapper = (h2_runtime_component_mapper_t){
        .user = env,
        .vtable = &component_mapper_vtable,
    };
    static const h2_pal_button_vtable_t button_vtable = {
        .read_single_button = test_read_single_button,
        .read_radio_button_group = test_read_radio_group,
    };
    env->button = (h2_pal_button_api_t){ .user = &env->button_state, .vtable = &button_vtable };
    static const h2_pal_nfc_vtable_t nfc_vtable = {
        .scan_nfc_reader = test_nfc_scan,
        .read_nfc_data = test_nfc_read,
    };
    env->nfc = (h2_pal_nfc_api_t){ .user = &env->nfc_state, .vtable = &nfc_vtable };
    static const h2_pal_nfc_card_emulation_vtable_t
        nfc_card_emulation_vtable = {
            .get_capabilities =
                test_nfc_card_emulation_get_capabilities,
        };
    env->nfc_card_emulation = (h2_pal_nfc_card_emulation_api_t){
        .user = &env->nfc_card_emulation_state,
        .vtable = &nfc_card_emulation_vtable,
    };
    static const h2_pal_imu_vtable_t imu_vtable = {
        .read_imu = test_imu_read,
    };
    env->imu = (h2_pal_imu_api_t){ .user = &env->imu_state, .vtable = &imu_vtable };
    static const h2_pal_input_vtable_t input_vtable = {
        .read_battery = test_input_read_battery,
        .read_temperature = test_input_read_temperature,
    };
    env->input = (h2_pal_input_api_t){
        .user = &env->input_state,
        .vtable = &input_vtable,
    };
    static const h2_pal_task_vtable_t task_vtable = {
        .start = test_task_start,
        .join = test_task_join,
    };
    env->task = (h2_pal_task_api_t){
        .user = &env->task_state,
        .vtable = &task_vtable,
    };
    static const h2_pal_sync_vtable_t sync_vtable = {
        .create_mutex = test_sync_create_mutex,
        .destroy_mutex = test_sync_destroy_mutex,
        .lock_mutex = test_sync_lock_mutex,
        .unlock_mutex = test_sync_unlock_mutex,
        .create_cond = test_sync_create_cond,
        .destroy_cond = test_sync_destroy_cond,
        .wait_cond = test_sync_wait_cond,
        .broadcast_cond = test_sync_broadcast_cond,
    };
    env->sync = (h2_pal_sync_api_t){ .user = &env->sync_state, .vtable = &sync_vtable };
    static const h2_pal_system_event_vtable_t system_event_vtable = {
        .init = test_system_event_init,
        .deinit = test_system_event_deinit,
        .subscribe = test_system_event_subscribe,
        .unsubscribe = test_system_event_unsubscribe,
    };
    env->system_event = (h2_pal_system_event_api_t){
        .user = &env->system_event_state,
        .vtable = &system_event_vtable,
    };
}

static h2_runtime_config_t test_runtime_config(test_runtime_env_t *env) {
    return (h2_runtime_config_t){
        .board = "test-board",
        .target = "host",
        .chip = "host",
        .firmware_info = h2_pal_unsupported_firmware_info_api(),
        .log = h2_pal_unsupported_log_api(),
        .time = &env->time,
        .timer = h2_pal_unsupported_timer_api(),
        .queue = &env->queue,
        .fs = h2_pal_unsupported_fs_api(),
        .disk = h2_pal_unsupported_disk_api(),
        .pref = h2_pal_unsupported_pref_api(),
        .crypto = h2_pal_unsupported_crypto_api(),
        .http = h2_pal_unsupported_http_api(),
        .net = h2_pal_unsupported_net_api(),
        .netif = h2_pal_unsupported_netif_api(),
        .mqtt = h2_pal_unsupported_mqtt_api(),
        .webrtc = h2_pal_unsupported_webrtc_api(),
        .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
        .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
        .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
        .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
        .ble_host = h2_pal_unsupported_ble_host_api(),
        .modem = h2_pal_unsupported_modem_api(),
        .power = h2_pal_unsupported_power_api(),
        .display = h2_pal_unsupported_display_api(),
        .audio = h2_pal_unsupported_audio_api(),
        .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
        .periph = &env->periph,
        .button = &env->button,
        .touch = h2_pal_unsupported_touch_api(),
        .buzzer = h2_pal_unsupported_buzzer_api(),
        .nfc = &env->nfc,
        .nfc_card_emulation = &env->nfc_card_emulation,
        .imu = &env->imu,
        .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
        .led = h2_pal_unsupported_led_api(),
        .switch_api = h2_pal_unsupported_switch_api(),
        .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
        .input = &env->input,
        .task = &env->task,
        .sync = &env->sync,
        .mem = &env->mem,
        .system_event = h2_pal_unsupported_system_event_api(),
        .video_decoder = h2_pal_unsupported_video_decoder_api(),
        .component_mapper = &env->component_mapper,
        .event_queue_capacity = 4u,
    };
}

static h2_runtime_t *test_runtime_create(test_runtime_env_t *env) {
    h2_runtime_config_t config = test_runtime_config(env);
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    assert(runtime != NULL);
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
    return runtime;
}

static h2_runtime_t *test_runtime_create_with_system_events(test_runtime_env_t *env) {
    h2_runtime_config_t config = test_runtime_config(env);
    config.system_event = &env->system_event;
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    assert(runtime != NULL);
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
    return runtime;
}

static h2_pal_result_t test_firmware_info_get_current(
    void *user,
    h2_pal_firmware_info_t *out_info) {
    const char *version = (const char *)user;

    memcpy(out_info->version, version, strlen(version) + 1u);
    return H2_PAL_OK;
}

static void test_runtime_firmware_info_provider(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    static const h2_pal_firmware_info_vtable_t firmware_info_vtable = {
        .get_current = test_firmware_info_get_current,
    };
    h2_pal_firmware_info_api_t firmware_info_api = {
        .user = (void *)"runtime-version",
        .vtable = &firmware_info_vtable,
    };
    h2_runtime_config_t config = test_runtime_config(&env);
    config.firmware_info = &firmware_info_api;
    h2_runtime_t *runtime = NULL;

    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    assert(runtime->firmware_info != &firmware_info_api);
    h2_pal_firmware_info_t firmware_info;
    assert(h2_pal_firmware_info_get_current(
               runtime->firmware_info,
               &firmware_info) == H2_PAL_OK);
    assert(strcmp(firmware_info.version, "runtime-version") == 0);
    h2_runtime_deinit(runtime);

    config.firmware_info = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_INVALID_ARG);
    assert(runtime == NULL);
}

static void test_runtime_capabilities_are_bound_at_init(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_t *runtime = test_runtime_create(&env);

    assert(runtime->mem != &env.mem);
    assert(runtime->firmware_info != h2_pal_unsupported_firmware_info_api());
    assert(runtime->firmware_info->vtable ==
           h2_pal_unsupported_firmware_info_api()->vtable);
    assert(runtime->mem->user == env.mem.user);
    assert(runtime->mem->vtable == env.mem.vtable);
    assert(runtime->time != &env.time && runtime->time->vtable == env.time.vtable);
    assert(runtime->queue != &env.queue && runtime->queue->vtable == env.queue.vtable);
    assert(runtime->task != &env.task && runtime->task->vtable == env.task.vtable);
    assert(runtime->sync != &env.sync && runtime->sync->vtable == env.sync.vtable);
    assert(runtime->periph != &env.periph && runtime->periph->vtable == env.periph.vtable);
    assert(runtime->button != &env.button && runtime->button->vtable == env.button.vtable);
    assert(runtime->buzzer != h2_pal_unsupported_buzzer_api());
    assert(runtime->buzzer->vtable == h2_pal_unsupported_buzzer_api()->vtable);
    assert(runtime->nfc != &env.nfc && runtime->nfc->vtable == env.nfc.vtable);
    assert(runtime->nfc_card_emulation != &env.nfc_card_emulation);
    assert(runtime->nfc_card_emulation->user ==
           env.nfc_card_emulation.user);
    assert(runtime->nfc_card_emulation->vtable ==
           env.nfc_card_emulation.vtable);
    assert(runtime->imu != &env.imu && runtime->imu->vtable == env.imu.vtable);
    assert(runtime->display != h2_pal_unsupported_display_api());
    assert(runtime->display->vtable == h2_pal_unsupported_display_api()->vtable);
    assert(runtime->system_event != h2_pal_unsupported_system_event_api());
    assert(runtime->system_event->vtable == h2_pal_unsupported_system_event_api()->vtable);
    assert(runtime->video_decoder != h2_pal_unsupported_video_decoder_api());
    assert(runtime->video_decoder->vtable == h2_pal_unsupported_video_decoder_api()->vtable);
    assert(runtime->wifi_csi != h2_pal_unsupported_wifi_csi_api());
    assert(runtime->wifi_csi->vtable == h2_pal_unsupported_wifi_csi_api()->vtable);
    assert(runtime->private_state != NULL);
    assert(runtime->private_state->input_source_capacity ==
           H2_RUNTIME_DEFAULT_INPUT_SOURCE_CAPACITY);
    assert(runtime->private_state->component_mapping_capacity ==
           H2_RUNTIME_DEFAULT_COMPONENT_MAPPING_CAPACITY);
    assert(runtime->private_state->event_payload_capacity ==
           H2_RUNTIME_EVENT_PAYLOAD_MAX);

    h2_pal_nfc_card_emulation_capabilities_t capabilities;
    assert(h2_pal_nfc_card_emulation_get_capabilities(
               runtime->nfc_card_emulation,
               1u,
               &capabilities) == H2_PAL_OK);
    assert(env.nfc_card_emulation_state.capability_calls == 1u);
    assert(env.nfc_card_emulation_state.last_periph_id == 1u);

    h2_runtime_deinit(runtime);
}

static void test_runtime_rejects_incomplete_video_decoder(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    static const h2_pal_video_decoder_vtable_t incomplete_vtable = {0};
    const h2_pal_video_decoder_api_t incomplete_api = {
        .user = NULL,
        .vtable = &incomplete_vtable,
    };
    h2_runtime_config_t config = test_runtime_config(&env);
    h2_runtime_t *runtime = (h2_runtime_t *)(uintptr_t)1u;

    config.video_decoder = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_INVALID_ARG);
    assert(runtime == NULL);

    runtime = (h2_runtime_t *)(uintptr_t)1u;
    config.video_decoder = &incomplete_api;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_INVALID_ARG);
    assert(runtime == NULL);
}

static void test_runtime_rejects_missing_wifi_csi(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_config_t config = test_runtime_config(&env);
    h2_runtime_t *runtime = (h2_runtime_t *)(uintptr_t)1u;

    config.wifi_csi = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_INVALID_ARG);
    assert(runtime == NULL);
}

static void test_runtime_rejects_missing_nfc_card_emulation(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_config_t config = test_runtime_config(&env);
    h2_runtime_t *runtime = (h2_runtime_t *)(uintptr_t)1u;

    config.nfc_card_emulation = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_INVALID_ARG);
    assert(runtime == NULL);
}

static void test_runtime_buzzer_contract(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_config_t config = test_runtime_config(&env);
    config.buzzer = h2_runtime_test_buzzer_api();
    h2_runtime_t *runtime = NULL;

    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    h2_runtime_test_buzzer_binding(runtime);
    h2_runtime_deinit(runtime);

    config = test_runtime_config(&env);
    config.buzzer = NULL;
    runtime = (h2_runtime_t *)(uintptr_t)1u;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_INVALID_ARG);
    assert(runtime == NULL);

    config = test_runtime_config(&env);
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    h2_pal_buzzer_info_t info;
    assert(h2_pal_buzzer_get_info(runtime->buzzer, 1u, &info) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_buzzer_start(runtime->buzzer, 1u, 440u, 50u) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_buzzer_stop(runtime->buzzer, 1u) == H2_PAL_ERR_UNSUPPORTED);
    h2_runtime_deinit(runtime);
}

static void test_runtime_binds_unsupported_nfc_card_emulation(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_config_t config = test_runtime_config(&env);
    config.nfc_card_emulation =
        h2_pal_unsupported_nfc_card_emulation_api();
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    h2_pal_nfc_card_emulation_capabilities_t capabilities;
    assert(h2_pal_nfc_card_emulation_get_capabilities(
               runtime->nfc_card_emulation,
               1u,
               &capabilities) == H2_PAL_ERR_UNSUPPORTED);
    h2_runtime_deinit(runtime);
}

static void test_runtime_private_allocation_failure_cleans_up(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    env.allocator_state.fail_on_call = 2u;
    env.allocator_state.fill_allocations_nonzero = 1;
    h2_runtime_config_t config = test_runtime_config(&env);
    h2_runtime_t *runtime = (h2_runtime_t *)(uintptr_t)1u;

    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_NO_MEMORY);
    assert(runtime == NULL);
    assert(env.allocator_state.alloc_calls == 2u);
    assert(env.allocator_state.free_calls == 1u);
    assert(env.allocator_state.live_allocations == 0u);
}

static void add_periph(
    test_runtime_env_t *env,
    h2_pal_periph_id_t periph_id,
    h2_pal_periph_type_t type,
    const void *payload,
    size_t payload_size) {
    env->periphs.infos[env->periphs.count++] = (h2_pal_periph_info_t){
        .id = periph_id,
        .type = type,
        .payload = payload,
        .payload_size = payload_size,
    };
}

static void test_runtime_uses_initialization_capacities(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    const size_t payload_capacity =
        h2_runtime_system_event_payload_capacity_min();
    h2_runtime_config_t config = test_runtime_config(&env);
    config.input_source_capacity = 3u;
    config.component_mapping_capacity = 6u;
    config.event_payload_capacity = payload_capacity;
    h2_runtime_t *runtime = NULL;

    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    assert(runtime->private_state->input_source_capacity == 3u);
    assert(runtime->private_state->component_mapping_capacity == 6u);
    assert(runtime->private_state->input_pending_event_capacity == 3u);
    assert(runtime->private_state->event_payload_capacity == payload_capacity);
    assert(runtime->private_state->event_queue->item_size ==
           offsetof(h2_runtime_queued_event_t, payload) + payload_capacity);
    assert((uintptr_t)runtime->private_state->component_mappings %
               _Alignof(h2_runtime_component_mapping_t) ==
           0u);
    assert((uintptr_t)runtime->private_state->input_sources %
               _Alignof(h2_runtime_input_source_t) ==
           0u);
    assert((uintptr_t)runtime->private_state->input_pending_events %
               _Alignof(h2_runtime_input_pending_event_t) ==
           0u);
    for (size_t index = 0u;
         index < H2_RUNTIME_STATE_SLOT_COUNT;
         ++index) {
        assert(runtime->private_state->state_publication.banks[index]
                   .entry_capacity == 3u);
        assert((uintptr_t)runtime->private_state->state_publication
                       .banks[index]
                       .entries %
                   _Alignof(h2_runtime_state_entry_t) ==
               0u);
    }

    h2_runtime_deinit(runtime);
    assert(env.allocator_state.live_allocations == 0u);
}

static void test_runtime_rejects_invalid_initialization_capacities(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_config_t config = test_runtime_config(&env);
    config.event_payload_capacity =
        h2_runtime_system_event_payload_capacity_min() - 1u;
    h2_runtime_t *runtime = (h2_runtime_t *)(uintptr_t)1u;

    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_INVALID_ARG);
    assert(runtime == NULL);
    assert(env.allocator_state.alloc_calls == 0u);

    config.event_payload_capacity = H2_RUNTIME_EVENT_PAYLOAD_MAX + 1u;
    runtime = (h2_runtime_t *)(uintptr_t)1u;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_INVALID_ARG);
    assert(runtime == NULL);
    assert(env.allocator_state.alloc_calls == 0u);

    config.event_payload_capacity =
        h2_runtime_system_event_payload_capacity_min();
    config.input_source_capacity = SIZE_MAX;
    runtime = (h2_runtime_t *)(uintptr_t)1u;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_INVALID_ARG);
    assert(runtime == NULL);
    assert(env.allocator_state.alloc_calls == 0u);
}

static void test_runtime_allocation_failures_release_owned_memory(void) {
    for (size_t fail_on_call = 1u; fail_on_call <= 4u; ++fail_on_call) {
        test_runtime_env_t env;
        test_env_init(&env);
        add_periph(&env, 401u, H2_PAL_PERIPH_TYPE_BATTERY, NULL, 0u);
        env.allocator_state.fail_on_call = fail_on_call;
        h2_runtime_config_t config = test_runtime_config(&env);
        h2_runtime_t *runtime = (h2_runtime_t *)(uintptr_t)1u;

        assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_NO_MEMORY);
        assert(runtime == NULL);
        assert(env.allocator_state.live_allocations == 0u);
    }
}

static void test_runtime_mapping_queries_only_mapped_peripherals(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    for (h2_pal_periph_id_t id = 1u; id <= 7u; ++id) {
        add_periph(&env, id, H2_PAL_PERIPH_TYPE_LED_STRIP, NULL, 0u);
    }
    add_periph(&env, 401u, H2_PAL_PERIPH_TYPE_BATTERY, NULL, 0u);
    h2_runtime_config_t config = test_runtime_config(&env);
    config.component_mapping_capacity = 1u;
    h2_runtime_t *runtime = NULL;

    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    assert(runtime->private_state->component_mapping_count == 1u);
    assert(h2_runtime_find_component_mapping_by_periph(runtime, 401u) !=
           NULL);
    assert(env.periphs.get_calls == 1u);
    /* Init resolves mapper entries and discovers input sources once. */
    assert(env.periphs.list_calls == 1u);
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
    assert(env.periphs.get_calls == 1u);
    assert(env.periphs.list_calls == 1u);
    h2_runtime_deinit(runtime);
    assert(env.allocator_state.live_allocations == 0u);
}

static void test_runtime_reports_configured_capacity_exhaustion(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 1u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    add_periph(&env, 2u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_config_t config = test_runtime_config(&env);
    config.component_mapping_capacity = 1u;
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_NO_SPACE);
    assert(runtime == NULL);
    assert(env.allocator_state.live_allocations == 0u);

    test_env_init(&env);
    add_periph(&env, 1u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    add_periph(&env, 2u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    /*
     * Input source discovery runs during init, so capacity exhaustion is an
     * initialization failure rather than something a later start reports.
     */
    config = test_runtime_config(&env);
    config.component_mapping_capacity = 2u;
    config.input_source_capacity = 1u;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_NO_SPACE);
    assert(runtime == NULL);
    assert(env.allocator_state.live_allocations == 0u);
}

static void test_runtime_payload_capacity_only_limits_enqueue(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    const size_t system_event_capacity =
        h2_runtime_system_event_payload_capacity_min();
    const size_t payload_capacity =
        system_event_capacity > 528u ? system_event_capacity : 528u;
    h2_runtime_config_t config = test_runtime_config(&env);
    config.event_payload_capacity = payload_capacity;
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    for (size_t index = 0u; index < sizeof(payload); ++index) {
        payload[index] = (uint8_t)index;
    }
    assert(h2_runtime_emit_event(
               runtime,
               H2_RUNTIME_SYSTEM_EVENT_MODEM_READY,
               H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
               H2_RUNTIME_COMPONENT_ID_NONE,
               1u,
               2u,
               payload,
               payload_capacity) == H2_PAL_OK);
    assert(h2_runtime_emit_event(
               runtime,
               H2_RUNTIME_SYSTEM_EVENT_MODEM_READY,
               H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
               H2_RUNTIME_COMPONENT_ID_NONE,
               2u,
               3u,
               payload,
               payload_capacity + 1u) == H2_PAL_ERR_INVALID_ARG);

    uint8_t selected_capacity_buffer[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {
        .payload = selected_capacity_buffer,
        .payload_capacity = payload_capacity,
    };
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_TRUNCATED);

    uint8_t app_buffer[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    event = (h2_runtime_event_t){
        .payload = app_buffer,
        .payload_capacity = sizeof(app_buffer),
    };
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.payload_size == payload_capacity);
    assert(memcmp(event.payload, payload, payload_capacity) == 0);

    h2_runtime_deinit(runtime);
    assert(env.allocator_state.live_allocations == 0u);
}


static h2_runtime_event_t event_with_payload(uint8_t *payload) {
    return (h2_runtime_event_t){
        .payload = payload,
        .payload_capacity = H2_RUNTIME_EVENT_PAYLOAD_MAX,
    };
}

static void assert_system_event_mapping(
    test_runtime_env_t *env,
    h2_runtime_t *runtime,
    h2_pal_system_event_type_t pal_type,
    const void *pal_payload,
    size_t pal_payload_size,
    h2_runtime_component_t component,
    h2_runtime_event_kind_t kind,
    size_t runtime_payload_size) {
    assert(test_system_event_dispatch(env, pal_type, pal_payload, pal_payload_size) == H2_PAL_OK);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.component == component);
    assert(event.component_id == H2_RUNTIME_COMPONENT_ID_NONE);
    assert(event.kind == kind);
    assert(event.sequence != 0u);
    assert(event.payload_size == runtime_payload_size);
}

static h2_pal_wifi_sta_status_t test_wifi_sta_status(void) {
    h2_pal_wifi_sta_status_t status;
    memset(&status, 0, sizeof(status));
    status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
    memcpy(status.ssid, "ssid", 4u);
    status.ssid_len = 4u;
    status.bssid[0] = 0xaa;
    status.bssid_set = 1u;
    status.channel = 6u;
    status.rssi = -45;
    status.ip.ip4 = 0x01020304u;
    status.ip_valid = 1u;
    return status;
}

static h2_pal_wifi_ap_event_t test_wifi_ap_event(void) {
    h2_pal_wifi_ap_event_t event;
    memset(&event, 0, sizeof(event));
    event.status.state = H2_PAL_WIFI_AP_STATE_STARTED;
    memcpy(event.status.ssid, "ap", 2u);
    event.status.ssid_len = 2u;
    event.status.channel = 11u;
    event.status.max_clients = 4u;
    event.status.client_count = 1u;
    event.status.security = H2_PAL_WIFI_SECURITY_WPA2;
    return event;
}

static h2_pal_wifi_ap_client_event_t test_wifi_client_event(void) {
    h2_pal_wifi_ap_client_event_t event;
    memset(&event, 0, sizeof(event));
    event.client.mac[0] = 0x10u;
    event.client.rssi = -50;
    event.client.lease.ip4 = 0x0a000002u;
    event.client.lease_valid = 1u;
    event.client.station_id = 3;
    return event;
}

static h2_pal_ble_connection_t test_ble_connection(void) {
    h2_pal_ble_connection_t connection;
    memset(&connection, 0, sizeof(connection));
    connection.conn_handle = 7u;
    connection.role = H2_PAL_BLE_ROLE_CENTRAL;
    connection.peer_addr.type = H2_PAL_BLE_ADDR_TYPE_RANDOM;
    connection.peer_addr.value[0] = 0x22u;
    connection.mtu = 247u;
    return connection;
}

static h2_pal_ble_gatt_client_value_t test_ble_gatt_value(void) {
    h2_pal_ble_gatt_client_value_t value;
    memset(&value, 0, sizeof(value));
    value.conn_handle = 7u;
    value.attr_handle = 9u;
    value.value_len = 2u;
    value.value[0] = 0xabu;
    value.value[1] = 0xcdu;
    return value;
}

static h2_pal_modem_status_t test_modem_status(void) {
    h2_pal_modem_status_t status;
    memset(&status, 0, sizeof(status));
    status.capabilities = H2_PAL_MODEM_CAPABILITY_DATA | H2_PAL_MODEM_CAPABILITY_CALL;
    status.sim = H2_PAL_MODEM_SIM_STATE_READY;
    status.registration = H2_PAL_MODEM_REGISTRATION_HOME;
    status.packet = H2_PAL_MODEM_PACKET_CONNECTED;
    status.rat = H2_PAL_MODEM_RAT_LTE;
    return status;
}

static h2_pal_modem_call_event_t test_modem_call_event(void) {
    h2_pal_modem_call_event_t event;
    memset(&event, 0, sizeof(event));
    event.call.call_id = 2;
    event.call.direction = H2_PAL_MODEM_CALL_DIRECTION_INCOMING;
    event.call.state = H2_PAL_MODEM_CALL_STATE_ACTIVE;
    memcpy(event.call.number, "+123", 5u);
    return event;
}

static void test_system_event_lifecycle_and_gpio_payload_copy(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    env.time_state.now_ms = 55u;
    h2_runtime_t *runtime = test_runtime_create_with_system_events(&env);
    assert(env.system_event_state.init_calls == 1u);
    assert(env.system_event_state.subscribe_calls ==
           (size_t)H2_PAL_SYSTEM_EVENT_TYPE_COUNT - 1u);

    h2_pal_gpio_irq_event_t pal = {
        .trigger = H2_PAL_GPIO_IRQ_TRIGGER_RISING,
    };
    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_GPIO_IRQ,
        &pal,
        sizeof(pal)) == H2_PAL_OK);
    pal.trigger = H2_PAL_GPIO_IRQ_TRIGGER_FALLING;

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.component == H2_RUNTIME_COMPONENT_SYSTEM_GPIO_IRQ);
    assert(event.component_id == H2_RUNTIME_COMPONENT_ID_NONE);
    assert(event.kind == H2_RUNTIME_SYSTEM_EVENT_GPIO_IRQ_TRIGGERED);
    assert(event.sequence == 1u);
    assert(event.timestamp_ms == 55u);
    assert(event.payload_size == sizeof(h2_runtime_system_event_gpio_irq_t));
    const h2_runtime_system_event_gpio_irq_t *gpio =
        (const h2_runtime_system_event_gpio_irq_t *)event.payload;
    assert(gpio->trigger == H2_RUNTIME_SYSTEM_GPIO_IRQ_TRIGGER_RISING);

    h2_runtime_system_gpio_irq_state_t state;
    assert(h2_runtime_system_state_gpio_irq(runtime, &state) == H2_PAL_ERR_UNSUPPORTED);

    h2_pal_system_event_subscription_t old_subscription = env.system_event_state.subscriptions[0];
    h2_runtime_stop_system_events(runtime);
    assert(env.system_event_state.unsubscribe_calls ==
           (size_t)H2_PAL_SYSTEM_EVENT_TYPE_COUNT - 1u);
    assert(env.system_event_state.deinit_calls == 1u);
    h2_pal_system_event_t event_after_stop = {
        .type = H2_PAL_SYSTEM_EVENT_TYPE_GPIO_IRQ,
        .payload = &pal,
        .payload_size = sizeof(pal),
    };
    assert(old_subscription.handler(old_subscription.handler_user, &event_after_stop) ==
           H2_PAL_ERR_CLOSED);

    h2_runtime_deinit(runtime);
    assert(env.system_event_state.deinit_calls == 1u);
}

static void test_system_event_maximum_ble_value_is_copied(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    const size_t system_event_capacity =
        h2_runtime_system_event_payload_capacity_min();
    const size_t payload_capacity =
        system_event_capacity > 528u ? system_event_capacity : 528u;
    h2_runtime_config_t config = test_runtime_config(&env);
    config.system_event = &env.system_event;
    config.event_payload_capacity = payload_capacity;
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    assert(runtime->private_state->event_payload_capacity == payload_capacity);
    assert(runtime->private_state->event_queue->item_size ==
           offsetof(h2_runtime_queued_event_t, payload) + payload_capacity);
    h2_pal_ble_gatt_client_value_t pal;
    memset(&pal, 0, sizeof(pal));
    pal.conn_handle = 17u;
    pal.attr_handle = 29u;
    pal.value_len = H2_RUNTIME_SYSTEM_BLE_ATT_MAX_VALUE_LEN;
    for (size_t i = 0u; i < pal.value_len; ++i) {
        pal.value[i] = (uint8_t)(i ^ 0xa5u);
    }

    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION,
        &pal,
        sizeof(pal)) == H2_PAL_OK);
    memset(pal.value, 0, sizeof(pal.value));

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = {
        .payload = payload,
        .payload_capacity = sizeof(payload),
    };
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.payload_size ==
           sizeof(h2_runtime_system_event_ble_gatt_client_value_t));
    const h2_runtime_system_event_ble_gatt_client_value_t *value =
        (const h2_runtime_system_event_ble_gatt_client_value_t *)event.payload;
    assert(value->conn_handle == 17u);
    assert(value->attr_handle == 29u);
    assert(value->value_len == H2_RUNTIME_SYSTEM_BLE_ATT_MAX_VALUE_LEN);
    for (size_t i = 0u; i < value->value_len; ++i) {
        assert(value->value[i] == (uint8_t)(i ^ 0xa5u));
    }

    h2_runtime_deinit(runtime);
}

static void test_system_event_partial_subscribe_cleans_up(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    env.system_event_state.fail_subscribe_after = 2u;
    h2_runtime_config_t config = test_runtime_config(&env);
    config.system_event = &env.system_event;
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_ERR_IO);
    assert(runtime == NULL);
    assert(env.system_event_state.unsubscribe_calls == 2u);
    assert(env.system_event_state.deinit_calls == 1u);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_system_event_projects_all_scope_events(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_t *runtime = test_runtime_create_with_system_events(&env);

    h2_pal_gpio_irq_event_t gpio = { .trigger = H2_PAL_GPIO_IRQ_TRIGGER_HIGH };
    h2_pal_wifi_sta_status_t sta = test_wifi_sta_status();
    h2_pal_wifi_ap_event_t ap = test_wifi_ap_event();
    h2_pal_wifi_ap_client_event_t client = test_wifi_client_event();
    h2_pal_ble_connection_t ble_connection = test_ble_connection();
    h2_pal_ble_connection_params_t ble_params = {
        .interval_min_ms = 10u,
        .interval_max_ms = 20u,
        .latency = 1u,
        .supervision_timeout_ms = 400u,
    };
    h2_pal_ble_disconnected_info_t ble_disconnected = {
        .conn_handle = 7u,
        .reason = 19,
    };
    h2_pal_ble_mtu_info_t ble_mtu = {
        .conn_handle = 7u,
        .mtu = 247u,
    };
    h2_pal_ble_subscription_state_t ble_subscription = {
        .conn_handle = 7u,
        .value_handle = 9u,
        .mode = H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY,
        .enabled = true,
    };
    h2_pal_ble_gatt_client_value_t ble_value = test_ble_gatt_value();
    uint8_t advertising_set_identity = 0u;
    h2_pal_ble_adv_set_event_t ble_advertising = {
        .set = (h2_pal_ble_adv_set_t *)&advertising_set_identity,
        .status = H2_PAL_ERR_IO,
    };
    h2_pal_modem_event_t modem_error = {
        .result = H2_PAL_ERR_IO,
        .vendor_code = 12,
    };
    h2_pal_modem_status_t modem_status = test_modem_status();
    h2_pal_modem_signal_t modem_signal = {
        .rssi_dbm = -70,
        .ber = 1,
        .rat = H2_PAL_MODEM_RAT_LTE,
    };
    h2_pal_modem_data_status_t modem_data = {
        .state = H2_PAL_MODEM_DATA_OPEN,
        .ip4 = 0x0a000003u,
        .ip4_valid = 1u,
    };
    h2_pal_modem_call_event_t modem_call = test_modem_call_event();
    h2_pal_netif_default_changed_t netif_change;
    memset(&netif_change, 0, sizeof(netif_change));
    netif_change.previous.type = H2_PAL_NETIF_REF_NAME;
    netif_change.previous.kind = H2_PAL_NETIF_KIND_WIFI_STA;
    memcpy(netif_change.previous.name, "en0", 4u);
    netif_change.previous_valid = 1u;
    netif_change.current.type = H2_PAL_NETIF_REF_ID;
    netif_change.current.kind = H2_PAL_NETIF_KIND_MODEM_DATA;
    netif_change.current.id = 9u;
    netif_change.current_valid = 1u;

    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_GPIO_IRQ, &gpio, sizeof(gpio),
        H2_RUNTIME_COMPONENT_SYSTEM_GPIO_IRQ, H2_RUNTIME_SYSTEM_EVENT_GPIO_IRQ_TRIGGERED,
        sizeof(h2_runtime_system_event_gpio_irq_t));

    const h2_pal_system_event_type_t sta_types[] = {
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING,
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED,
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED,
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_GOT_IP,
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP,
    };
    const h2_runtime_event_kind_t sta_kinds[] = {
        H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTING,
        H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED,
        H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_DISCONNECTED,
        H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP,
        H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP,
    };
    for (size_t i = 0u; i < sizeof(sta_types) / sizeof(sta_types[0]); ++i) {
        assert_system_event_mapping(
            &env, runtime, sta_types[i], &sta, sizeof(sta),
            H2_RUNTIME_COMPONENT_SYSTEM_WIFI, sta_kinds[i],
            sizeof(h2_runtime_system_event_wifi_sta_t));
    }

    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STARTED, &ap, sizeof(ap),
        H2_RUNTIME_COMPONENT_SYSTEM_WIFI, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STARTED,
        sizeof(h2_runtime_system_event_wifi_ap_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED, &ap, sizeof(ap),
        H2_RUNTIME_COMPONENT_SYSTEM_WIFI, H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_STOPPED,
        sizeof(h2_runtime_system_event_wifi_ap_t));

    const h2_pal_system_event_type_t client_types[] = {
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_JOINED,
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_LEFT,
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_GRANTED,
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_RELEASED,
    };
    const h2_runtime_event_kind_t client_kinds[] = {
        H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_JOINED,
        H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_CLIENT_LEFT,
        H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_GRANTED,
        H2_RUNTIME_SYSTEM_EVENT_WIFI_AP_LEASE_RELEASED,
    };
    for (size_t i = 0u; i < sizeof(client_types) / sizeof(client_types[0]); ++i) {
        assert_system_event_mapping(
            &env, runtime, client_types[i], &client, sizeof(client),
            H2_RUNTIME_COMPONENT_SYSTEM_WIFI, client_kinds[i],
            sizeof(h2_runtime_system_event_wifi_ap_client_t));
    }

    const h2_pal_system_event_type_t ble_no_payload_types[] = {
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STARTED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_SCAN_STOPPED,
    };
    const h2_runtime_event_kind_t ble_no_payload_kinds[] = {
        H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STARTED,
        H2_RUNTIME_SYSTEM_EVENT_BLE_HOST_STOPPED,
        H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STARTED,
        H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STOPPED,
        H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STARTED,
        H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STOPPED,
    };
    for (size_t i = 0u; i < sizeof(ble_no_payload_types) / sizeof(ble_no_payload_types[0]); ++i) {
        assert_system_event_mapping(
            &env, runtime, ble_no_payload_types[i], NULL, 0u,
            H2_RUNTIME_COMPONENT_SYSTEM_BLE, ble_no_payload_kinds[i], 0u);
    }
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
        &ble_advertising, sizeof(ble_advertising),
        H2_RUNTIME_COMPONENT_SYSTEM_BLE,
        H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STARTED,
        sizeof(h2_runtime_system_event_ble_advertising_set_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED, &ble_connection, sizeof(ble_connection),
        H2_RUNTIME_COMPONENT_SYSTEM_BLE, H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTED,
        sizeof(h2_runtime_system_event_ble_connection_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTION_UPDATED, &ble_params, sizeof(ble_params),
        H2_RUNTIME_COMPONENT_SYSTEM_BLE, H2_RUNTIME_SYSTEM_EVENT_BLE_CONNECTION_UPDATED,
        sizeof(h2_runtime_system_event_ble_connection_params_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED, &ble_disconnected, sizeof(ble_disconnected),
        H2_RUNTIME_COMPONENT_SYSTEM_BLE, H2_RUNTIME_SYSTEM_EVENT_BLE_DISCONNECTED,
        sizeof(h2_runtime_system_event_ble_disconnected_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED, &ble_mtu, sizeof(ble_mtu),
        H2_RUNTIME_COMPONENT_SYSTEM_BLE, H2_RUNTIME_SYSTEM_EVENT_BLE_MTU_CHANGED,
        sizeof(h2_runtime_system_event_ble_mtu_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_SUBSCRIPTION_CHANGED, &ble_subscription, sizeof(ble_subscription),
        H2_RUNTIME_COMPONENT_SYSTEM_BLE, H2_RUNTIME_SYSTEM_EVENT_BLE_SUBSCRIPTION_CHANGED,
        sizeof(h2_runtime_system_event_ble_subscription_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION, &ble_value, sizeof(ble_value),
        H2_RUNTIME_COMPONENT_SYSTEM_BLE, H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_NOTIFICATION,
        sizeof(h2_runtime_system_event_ble_gatt_client_value_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_INDICATION, &ble_value, sizeof(ble_value),
        H2_RUNTIME_COMPONENT_SYSTEM_BLE, H2_RUNTIME_SYSTEM_EVENT_BLE_GATT_CLIENT_INDICATION,
        sizeof(h2_runtime_system_event_ble_gatt_client_value_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_READY, NULL, 0u,
        H2_RUNTIME_COMPONENT_SYSTEM_MODEM, H2_RUNTIME_SYSTEM_EVENT_MODEM_READY, 0u);
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_ERROR, &modem_error, sizeof(modem_error),
        H2_RUNTIME_COMPONENT_SYSTEM_MODEM, H2_RUNTIME_SYSTEM_EVENT_MODEM_ERROR,
        sizeof(h2_runtime_system_event_modem_error_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIM_CHANGED, &modem_status, sizeof(modem_status),
        H2_RUNTIME_COMPONENT_SYSTEM_MODEM, H2_RUNTIME_SYSTEM_EVENT_MODEM_SIM_CHANGED,
        sizeof(h2_runtime_system_event_modem_sim_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_REGISTRATION_CHANGED, &modem_status, sizeof(modem_status),
        H2_RUNTIME_COMPONENT_SYSTEM_MODEM, H2_RUNTIME_SYSTEM_EVENT_MODEM_REGISTRATION_CHANGED,
        sizeof(h2_runtime_system_event_modem_registration_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_PACKET_CHANGED, &modem_status, sizeof(modem_status),
        H2_RUNTIME_COMPONENT_SYSTEM_MODEM, H2_RUNTIME_SYSTEM_EVENT_MODEM_PACKET_CHANGED,
        sizeof(h2_runtime_system_event_modem_packet_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIGNAL_CHANGED, &modem_signal, sizeof(modem_signal),
        H2_RUNTIME_COMPONENT_SYSTEM_MODEM, H2_RUNTIME_SYSTEM_EVENT_MODEM_SIGNAL_CHANGED,
        sizeof(h2_runtime_system_event_modem_signal_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_OPENED, &modem_data, sizeof(modem_data),
        H2_RUNTIME_COMPONENT_SYSTEM_MODEM, H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_OPENED,
        sizeof(h2_runtime_system_event_modem_data_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_CLOSED, &modem_data, sizeof(modem_data),
        H2_RUNTIME_COMPONENT_SYSTEM_MODEM, H2_RUNTIME_SYSTEM_EVENT_MODEM_DATA_CLOSED,
        sizeof(h2_runtime_system_event_modem_data_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING, &modem_call, sizeof(modem_call),
        H2_RUNTIME_COMPONENT_SYSTEM_MODEM, H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_INCOMING,
        sizeof(h2_runtime_system_event_modem_call_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED, &modem_call, sizeof(modem_call),
        H2_RUNTIME_COMPONENT_SYSTEM_MODEM, H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_STATE_CHANGED,
        sizeof(h2_runtime_system_event_modem_call_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED, &modem_call, sizeof(modem_call),
        H2_RUNTIME_COMPONENT_SYSTEM_MODEM, H2_RUNTIME_SYSTEM_EVENT_MODEM_CALL_ENDED,
        sizeof(h2_runtime_system_event_modem_call_t));
    assert_system_event_mapping(
        &env, runtime, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
        &netif_change, sizeof(netif_change),
        H2_RUNTIME_COMPONENT_SYSTEM_NETIF,
        H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED,
        sizeof(h2_runtime_system_event_netif_default_changed_t));

    h2_runtime_deinit(runtime);
}

static void test_system_event_modem_call_number_is_terminated(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_t *runtime = test_runtime_create_with_system_events(&env);

    h2_pal_modem_call_event_t modem_call = test_modem_call_event();
    memset(modem_call.call.number, '9', sizeof(modem_call.call.number));
    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING,
        &modem_call,
        sizeof(modem_call)) == H2_PAL_OK);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.payload_size == sizeof(h2_runtime_system_event_modem_call_t));

    const h2_runtime_system_event_modem_call_t *call =
        (const h2_runtime_system_event_modem_call_t *)event.payload;
    assert(call->number[H2_RUNTIME_SYSTEM_MODEM_PHONE_NUMBER_MAX] == '\0');
    for (size_t i = 0u; i < H2_RUNTIME_SYSTEM_MODEM_PHONE_NUMBER_MAX; ++i) {
        assert(call->number[i] == '9');
    }

    h2_runtime_deinit(runtime);
}

static h2_pal_netif_ref_t test_netif_name_ref(
    const char *name,
    h2_pal_netif_kind_t kind) {
    h2_pal_netif_ref_t ref;
    memset(&ref, 0, sizeof(ref));
    ref.type = H2_PAL_NETIF_REF_NAME;
    ref.kind = kind;
    size_t length = strlen(name);
    assert(length < sizeof(ref.name));
    memcpy(ref.name, name, length + 1u);
    return ref;
}

static void test_system_event_netif_transitions_copy_and_order(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_t *runtime = test_runtime_create_with_system_events(&env);
    const h2_pal_netif_ref_t wifi =
        test_netif_name_ref("wifi0", H2_PAL_NETIF_KIND_WIFI_STA);
    const h2_pal_netif_ref_t ppp =
        test_netif_name_ref("ppp0", H2_PAL_NETIF_KIND_MODEM_DATA);
    const struct {
        h2_pal_netif_ref_t previous;
        h2_pal_netif_ref_t current;
        uint8_t previous_valid;
        uint8_t current_valid;
    } transitions[] = {
        {.current = wifi, .current_valid = 1u},
        {.previous = wifi, .current = ppp,
         .previous_valid = 1u, .current_valid = 1u},
        {.previous = ppp, .current = wifi,
         .previous_valid = 1u, .current_valid = 1u},
        {.previous = wifi, .previous_valid = 1u},
    };
    h2_runtime_sequence_t last_sequence = 0u;
    for (size_t i = 0u; i < sizeof(transitions) / sizeof(transitions[0]); ++i) {
        h2_pal_netif_default_changed_t pal = {
            .previous = transitions[i].previous,
            .current = transitions[i].current,
            .previous_valid = transitions[i].previous_valid,
            .current_valid = transitions[i].current_valid,
        };
        assert(test_system_event_dispatch(
                   &env, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
                   &pal, sizeof(pal)) == H2_PAL_OK);
        memset(&pal, 0xa5, sizeof(pal));

        uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
        h2_runtime_event_t event = event_with_payload(payload);
        assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
        assert(event.component == H2_RUNTIME_COMPONENT_SYSTEM_NETIF);
        assert(event.kind == H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED);
        assert(event.sequence > last_sequence);
        last_sequence = event.sequence;
        const h2_runtime_system_event_netif_default_changed_t *change =
            (const h2_runtime_system_event_netif_default_changed_t *)
                event.payload;
        assert(change->previous_valid == transitions[i].previous_valid);
        assert(change->current_valid == transitions[i].current_valid);
        if (change->previous_valid != 0u) {
            assert(change->previous.name_valid == 1u);
            assert(strcmp(change->previous.name,
                          transitions[i].previous.name) == 0);
        }
        if (change->current_valid != 0u) {
            assert(change->current.name_valid == 1u);
            assert(strcmp(change->current.name,
                          transitions[i].current.name) == 0);
        }
    }
    h2_runtime_deinit(runtime);
}

static void test_system_event_rejects_invalid_payloads(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_t *runtime = test_runtime_create_with_system_events(&env);

    uint8_t bad_payload = 1u;
    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED,
        &bad_payload,
        sizeof(bad_payload)) == H2_PAL_ERR_INVALID_ARG);

    h2_pal_ble_gatt_client_value_t bad_ble = test_ble_gatt_value();
    bad_ble.value_len = H2_RUNTIME_SYSTEM_BLE_ATT_MAX_VALUE_LEN + 1u;
    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_GATT_CLIENT_NOTIFICATION,
        &bad_ble,
        sizeof(bad_ble)) == H2_PAL_ERR_INVALID_ARG);

    h2_pal_netif_default_changed_t bad_netif;
    memset(&bad_netif, 0, sizeof(bad_netif));
    bad_netif.current.type = H2_PAL_NETIF_REF_DEFAULT;
    bad_netif.current_valid = 1u;
    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
        &bad_netif,
        sizeof(bad_netif)) == H2_PAL_ERR_INVALID_ARG);
    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
        &bad_netif,
        sizeof(bad_netif) - 1u) == H2_PAL_ERR_INVALID_ARG);
    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
        &bad_netif,
        sizeof(bad_netif) + 1u) == H2_PAL_ERR_INVALID_ARG);
    bad_netif.current =
        test_netif_name_ref("bad0", (h2_pal_netif_kind_t)99);
    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
        &bad_netif,
        sizeof(bad_netif)) == H2_PAL_ERR_INVALID_ARG);
    bad_netif.current.kind = H2_PAL_NETIF_KIND_UNKNOWN;
    bad_netif.current_valid = 2u;
    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
        &bad_netif,
        sizeof(bad_netif)) == H2_PAL_ERR_INVALID_ARG);

    h2_pal_ble_adv_set_event_t bad_advertising = {
        .set = NULL,
        .status = H2_PAL_OK,
    };
    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
        &bad_advertising,
        sizeof(bad_advertising)) == H2_PAL_ERR_INVALID_ARG);
    assert(test_system_event_dispatch(
        &env,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
        &bad_payload,
        sizeof(bad_payload)) == H2_PAL_ERR_INVALID_ARG);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_WOULD_BLOCK);

    h2_runtime_deinit(runtime);
}

static void test_system_event_advertising_set_correlation_and_copy(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_t *runtime = test_runtime_create_with_system_events(&env);
    uint8_t first_identity = 0u;
    uint8_t second_identity = 0u;
    h2_pal_ble_adv_set_event_t first = {
        .set = (h2_pal_ble_adv_set_t *)&first_identity,
        .status = H2_PAL_OK,
    };
    h2_pal_ble_adv_set_event_t second = {
        .set = (h2_pal_ble_adv_set_t *)&second_identity,
        .status = H2_PAL_ERR_IO,
    };
    assert(test_system_event_dispatch(
        &env, H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
        &first, sizeof(first)) == H2_PAL_OK);
    assert(test_system_event_dispatch(
        &env, H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
        &second, sizeof(second)) == H2_PAL_OK);
    first.set = NULL;
    first.status = H2_PAL_ERR_IO;
    second.set = NULL;
    second.status = H2_PAL_OK;

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    const h2_runtime_system_event_ble_advertising_set_t *mapped =
        event.payload;
    assert(event.kind == H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STARTED);
    assert(mapped->set == &first_identity);
    assert(mapped->result == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    mapped = event.payload;
    assert(event.kind == H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STOPPED);
    assert(mapped->set == &second_identity);
    assert(mapped->result == H2_PAL_ERR_IO);
    h2_runtime_deinit(runtime);
}

static void test_system_event_advertising_queue_failure(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_t *runtime = test_runtime_create_with_system_events(&env);
    uint8_t identity = 0u;
    const h2_pal_ble_adv_set_event_t advertising = {
        .set = (h2_pal_ble_adv_set_t *)&identity,
        .status = H2_PAL_OK,
    };
    env.queue_state.send_rc = H2_PAL_ERR_IO;
    assert(test_system_event_dispatch(
        &env, H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STARTED,
        &advertising, sizeof(advertising)) == H2_PAL_ERR_IO);
    h2_runtime_deinit(runtime);
}

static void test_event_small_buffer_does_not_dequeue(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    env.button_state.single_state = H2_PAL_BUTTON_STATE_RELEASED;
    env.time_state.now_ms = H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);

    uint8_t tiny_payload[1];
    h2_runtime_event_t tiny = {
        .payload = tiny_payload,
        .payload_capacity = sizeof(tiny_payload),
    };
    assert(h2_runtime_poll_event(runtime, &tiny) == H2_PAL_ERR_TRUNCATED);

    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP);
    assert(event.component_id == 1u);

    h2_runtime_deinit(runtime);
}

static void test_event_queue_timeout_drops_event(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    env.queue_state.full_rc = H2_PAL_QUEUE_ERR_TIMEOUT;
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);

    for (size_t i = 0u; i < 5u; ++i) {
        env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
        env.time_state.now_ms += H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
        assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
        env.button_state.single_state = H2_PAL_BUTTON_STATE_RELEASED;
        env.time_state.now_ms += H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
        assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    }

    h2_runtime_deinit(runtime);
}

static void test_button_action_emits_on_release(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    env.time_state.now_ms = 10u;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    assert(event.timestamp_ms == 10u);
    assert(event.payload_size == sizeof(h2_runtime_button_down_event_t));
    h2_runtime_button_down_event_t down;
    memcpy(&down, event.payload, sizeof(down));
    assert(down.pressed_at_ms == 10u);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_RELEASED;
    env.time_state.now_ms = 40u;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP);
    assert(event.timestamp_ms == 40u);
    assert(event.payload_size == sizeof(h2_runtime_button_up_event_t));
    h2_runtime_button_up_event_t up;
    memcpy(&up, event.payload, sizeof(up));
    assert(up.pressed_at_ms == 10u);
    assert(up.released_at_ms == 40u);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
    assert(event.component == H2_RUNTIME_COMPONENT_BUTTON);
    assert(event.component_id == 1u);
    h2_runtime_button_action_event_t click;
    memcpy(&click, event.payload, sizeof(click));
    assert(click.click_count == 1u);

    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) == H2_PAL_OK);
    assert(!state.pressed);
    assert(state.pressed_at_ms == 0u);
    assert(state.updated_at_ms == 40u);
    assert(state.result == H2_PAL_OK);

    h2_runtime_deinit(runtime);
}

static void test_button_rapid_clicks_emit_separate_events(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    env.time_state.now_ms = 10u;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    env.button_state.single_state = H2_PAL_BUTTON_STATE_RELEASED;
    env.time_state.now_ms = 40u;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
    h2_runtime_button_action_event_t click;
    memcpy(&click, event.payload, sizeof(click));
    assert(click.click_count == 1u);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    env.time_state.now_ms = 40u + H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);

    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) == H2_PAL_OK);
    assert(state.pressed);
    assert(state.pressed_at_ms == env.time_state.now_ms);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_RELEASED;
    env.time_state.now_ms += H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
    memcpy(&click, event.payload, sizeof(click));
    assert(click.click_count == 2u);

    h2_runtime_deinit(runtime);
}

static void test_button_held_polls_stay_silent_and_release_emits_action(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    env.time_state.now_ms = 10u;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);

    /* Holding the button produces no further events; the pressed state
     * snapshot carries pressed_at_ms so Apps can measure the hold. */
    env.time_state.now_ms = 10u + H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_WOULD_BLOCK);
    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(state.pressed);
    assert(state.pressed_at_ms == 10u);
    assert(state.click_count == 1u);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_RELEASED;
    env.time_state.now_ms += H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
    h2_runtime_button_action_event_t action;
    memcpy(&action, event.payload, sizeof(action));
    assert(action.pressed_at_ms == 10u);
    assert(action.released_at_ms == 10u + 2u * H2_RUNTIME_BUTTON_POLL_INTERVAL_MS);
    assert(action.click_count == 1u);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_WOULD_BLOCK);
    /* The released snapshot keeps the count of the last completed action. */
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(!state.pressed);
    assert(state.click_count == 1u);

    /* A second press within the click gap continues the sequence in state. */
    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    env.time_state.now_ms += H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(state.pressed);
    assert(state.click_count == 2u);

    h2_runtime_deinit(runtime);
}

static void test_push_button_uses_mapping_and_runtime_gesture_recognizer(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    const h2_pal_periph_single_button_payload_t push_payload = {
        .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
    };
    add_periph(&env, 77u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
               &push_payload, sizeof(push_payload));
    h2_runtime_t *runtime = test_runtime_create(&env);

    h2_pal_periph_id_t periph_id = 0u;
    assert(h2_runtime_periph_id(runtime, 1u, &periph_id) == H2_PAL_OK);
    assert(periph_id == 77u);

    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(env.button_state.single_reads == 0u);

    env.time_state.now_ms = 10u;
    assert(h2_runtime_button_push_edge(
               runtime, 77u, H2_RUNTIME_BUTTON_EDGE_DOWN) == H2_PAL_OK);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    assert(event.component == H2_RUNTIME_COMPONENT_BUTTON);
    assert(event.component_id == 1u);

    env.time_state.now_ms = 10u + H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(env.button_state.single_reads == 0u);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_WOULD_BLOCK);

    env.time_state.now_ms += H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
    assert(h2_runtime_button_push_edge(
               runtime, 77u, H2_RUNTIME_BUTTON_EDGE_UP) == H2_PAL_OK);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
    assert(h2_runtime_poll_event(runtime, &event) ==
           H2_PAL_ERR_WOULD_BLOCK);

    env.time_state.now_ms += H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
    assert(h2_runtime_button_push_edge(
               runtime, 77u, H2_RUNTIME_BUTTON_EDGE_DOWN) == H2_PAL_OK);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    env.time_state.now_ms += 100u;
    assert(h2_runtime_button_push_edge(
               runtime, 77u, H2_RUNTIME_BUTTON_EDGE_UP) == H2_PAL_OK);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
    assert(h2_runtime_poll_event(runtime, &event) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_runtime_button_push_edge(
               runtime, 78u, H2_RUNTIME_BUTTON_EDGE_DOWN) ==
           H2_PAL_ERR_NOT_FOUND);
    assert(h2_runtime_button_push_edge(runtime, 77u, 0) ==
           H2_PAL_ERR_INVALID_ARG);

    h2_runtime_deinit(runtime);
}

static void test_test_control_discards_stale_push_edges(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    const h2_pal_periph_single_button_payload_t push_payload = {
        .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
    };
    add_periph(&env, 77u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
               &push_payload, sizeof(push_payload));
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);

    /* A physical edge is accepted but not yet consumed by the poller. */
    env.time_state.now_ms = 10u;
    assert(h2_runtime_button_push_edge(
               runtime, 77u, H2_RUNTIME_BUTTON_EDGE_DOWN) == H2_PAL_OK);

    /* Test control replaces the production source table. */
    h2_runtime_test_control_t *control = NULL;
    assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_OK);
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_WOULD_BLOCK);
    h2_runtime_test_control_close(control);

    /* The stale edge must neither fault the poller nor replay afterwards. */
    env.time_state.now_ms = 20u + H2_RUNTIME_BUTTON_POLL_INTERVAL_MS;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_WOULD_BLOCK);
    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(!state.pressed);

    h2_runtime_deinit(runtime);
}

static void test_button_push_rejects_non_push_and_invalid_payload(void) {
    test_runtime_env_t poll_env;
    test_env_init(&poll_env);
    add_periph(&poll_env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *poll_runtime = test_runtime_create(&poll_env);
    assert(h2_runtime_button_push_edge(
               poll_runtime, 10u, H2_RUNTIME_BUTTON_EDGE_DOWN) ==
           H2_PAL_ERR_NOT_FOUND);
    h2_runtime_deinit(poll_runtime);

    test_runtime_env_t invalid_env;
    test_env_init(&invalid_env);
    const h2_pal_periph_single_button_payload_t invalid_payload = {
        .delivery = (h2_pal_button_delivery_t)99,
    };
    add_periph(&invalid_env, 11u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
               &invalid_payload, sizeof(invalid_payload));
    h2_runtime_config_t invalid_config = test_runtime_config(&invalid_env);
    h2_runtime_t *invalid_runtime = NULL;
    /* Delivery mode is validated by init-time source discovery. */
    assert(h2_runtime_init(&invalid_config, &invalid_runtime) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(invalid_runtime == NULL);
    assert(invalid_env.allocator_state.live_allocations == 0u);
}

static void run_nfc_task_once(test_runtime_env_t *env,
                              h2_runtime_t *runtime) {
    assert(env->task_state.handle_count == 2u);
    env->time_state.stop_after_sleep_runtime = runtime;
    env->task_state.handles[1]->entry(env->task_state.handles[1]->ctx);
    env->time_state.stop_after_sleep_runtime = NULL;
    atomic_store(&runtime->private_state->input_stop_requested, 0);
}

static void test_nfc_discovery_and_state(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 20u, H2_PAL_PERIPH_TYPE_NFC_READER, NULL, 0u);
    env.nfc_state.scan = (h2_pal_nfc_scan_t){
        .stage = H2_PAL_NFC_STAGE_DISCOVERED,
        .result = H2_PAL_OK,
        .tag_type = H2_PAL_NFC_TAG_TYPE_NTAG,
        .uid_len = 4u,
        .uid = {1u, 2u, 3u, 4u},
    };
    env.nfc_state.read_bytes[0] = 0xaa;
    env.nfc_state.read_bytes[1] = 0xbb;
    h2_runtime_t *runtime = test_runtime_create(&env);
    run_nfc_task_once(&env, runtime);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(env.nfc_card_emulation_state.capability_calls == 0u);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_NFC_STATE);
    assert(event.component == H2_RUNTIME_COMPONENT_NFC_READER);
    assert(event.component_id == 1u);
    h2_runtime_nfc_state_t event_state;
    memcpy(&event_state, event.payload, sizeof(event_state));
    assert(event_state.status == H2_RUNTIME_NFC_STATE_DISCOVERED);
    assert(event_state.uid_len == 4u);
    assert(memcmp(event_state.uid, env.nfc_state.scan.uid, 4u) == 0);

    env.time_state.now_ms += H2_RUNTIME_NFC_POLL_INTERVAL_MS;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_WOULD_BLOCK);

    h2_runtime_nfc_state_t state;
    assert(h2_runtime_component_state_nfc(runtime, 1u, &state) == H2_PAL_OK);
    assert(state.status == H2_RUNTIME_NFC_STATE_DISCOVERED);
    assert(state.tag_type == H2_PAL_NFC_TAG_TYPE_NTAG);
    assert(state.uid_len == 4u);

    h2_runtime_deinit(runtime);
}

static void test_nfc_background_task_does_not_block_input_task(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 20u, H2_PAL_PERIPH_TYPE_NFC_READER, NULL, 0u);
    env.nfc_state.scan = (h2_pal_nfc_scan_t){
        .stage = H2_PAL_NFC_STAGE_DISCOVERED,
        .result = H2_PAL_OK,
        .uid_len = 1u,
        .uid = {0x42u},
    };
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(env.task_state.starts == 2u);
    assert(env.nfc_state.scan_calls == 0u);

    run_nfc_task_once(&env, runtime);
    assert(env.nfc_state.scan_calls == 1u);
    env.time_state.stop_after_sleep_runtime = runtime;
    env.task_state.handles[0]->entry(env.task_state.handles[0]->ctx);
    env.time_state.stop_after_sleep_runtime = NULL;
    atomic_store(&runtime->private_state->input_stop_requested, 0);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_NFC_STATE);
    h2_runtime_deinit(runtime);
    assert(env.task_state.joins == 2u);
}

static void test_imu_tilt_event_and_state(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 30u, H2_PAL_PERIPH_TYPE_IMU, NULL, 0u);
    env.imu_state.reading = (h2_pal_imu_reading_t){
        .flags = H2_PAL_IMU_HAS_ACCEL,
        .accel_mg = { .x = H2_RUNTIME_IMU_TILT_THRESHOLD_MG, .y = 0, .z = 1000 },
    };
    env.time_state.now_ms = H2_RUNTIME_IMU_TILT_DEBOUNCE_MS;
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE);
    assert(event.component == H2_RUNTIME_COMPONENT_IMU);
    assert(event.component_id == 1u);
    h2_runtime_imu_gesture_event_t gesture;
    memcpy(&gesture, event.payload, sizeof(gesture));
    assert(gesture.kind == H2_RUNTIME_IMU_GESTURE_TILT);
    assert(gesture.gesture.tilt.x_mg == H2_RUNTIME_IMU_TILT_THRESHOLD_MG);

    h2_runtime_imu_state_t state;
    assert(h2_runtime_component_state_imu(runtime, 1u, &state) == H2_PAL_OK);
    assert(state.gesture_kind == H2_RUNTIME_IMU_GESTURE_TILT);
    assert(state.gesture.tilt.x_mg == H2_RUNTIME_IMU_TILT_THRESHOLD_MG);

    h2_runtime_deinit(runtime);
}

static void test_imu_shake_suppresses_intermediate_tilt(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 30u, H2_PAL_PERIPH_TYPE_IMU, NULL, 0u);
    env.imu_state.reading = (h2_pal_imu_reading_t){
        .flags = H2_PAL_IMU_HAS_ACCEL,
        .accel_mg = { .x = H2_RUNTIME_IMU_SHAKE_THRESHOLD_MG, .y = 0, .z = 0 },
    };
    env.time_state.now_ms = H2_RUNTIME_IMU_TILT_DEBOUNCE_MS;
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_WOULD_BLOCK);

    env.time_state.now_ms += H2_RUNTIME_IMU_SHAKE_MIN_DURATION_MS;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_IMU_GESTURE);
    h2_runtime_imu_gesture_event_t gesture;
    memcpy(&gesture, event.payload, sizeof(gesture));
    assert(gesture.kind == H2_RUNTIME_IMU_GESTURE_SHAKE);

    h2_runtime_deinit(runtime);
}

static void test_imu_int32_min_sample_is_safe(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 30u, H2_PAL_PERIPH_TYPE_IMU, NULL, 0u);
    env.imu_state.reading = (h2_pal_imu_reading_t){
        .flags = H2_PAL_IMU_HAS_ACCEL,
        .accel_mg = { .x = INT32_MIN, .y = 0, .z = 0 },
    };
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    env.time_state.now_ms = H2_RUNTIME_IMU_SHAKE_MIN_DURATION_MS;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    h2_runtime_imu_gesture_event_t gesture;
    memcpy(&gesture, event.payload, sizeof(gesture));
    assert(gesture.kind == H2_RUNTIME_IMU_GESTURE_SHAKE);
    assert(gesture.gesture.shake.magnitude_mg == INT32_MAX);

    h2_runtime_deinit(runtime);
}

static void test_imu_ignores_samples_without_required_flags(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 30u, H2_PAL_PERIPH_TYPE_IMU, NULL, 0u);
    env.imu_state.reading = (h2_pal_imu_reading_t){
        .flags = H2_PAL_IMU_FLAG_NONE,
        .accel_mg = { .x = H2_RUNTIME_IMU_TILT_THRESHOLD_MG, .y = 0, .z = 0 },
        .gyro_mdps = { .x = 0, .y = 0, .z = H2_RUNTIME_IMU_FLIP_GYRO_THRESHOLD_MDPS },
    };
    env.time_state.now_ms = H2_RUNTIME_IMU_TILT_DEBOUNCE_MS;
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_WOULD_BLOCK);

    h2_runtime_imu_state_t state;
    assert(h2_runtime_component_state_imu(runtime, 1u, &state) == H2_PAL_OK);
    assert(state.gesture_kind == H2_RUNTIME_IMU_GESTURE_NONE);

    h2_runtime_deinit(runtime);
}

static void test_nfc_rejects_oversized_uid_len(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 20u, H2_PAL_PERIPH_TYPE_NFC_READER, NULL, 0u);
    env.nfc_state.scan = (h2_pal_nfc_scan_t){
        .stage = H2_PAL_NFC_STAGE_DISCOVERED,
        .result = H2_PAL_OK,
        .tag_type = H2_PAL_NFC_TAG_TYPE_NTAG,
        .uid_len = H2_PAL_NFC_UID_MAX_LEN + 1u,
    };
    h2_runtime_t *runtime = test_runtime_create(&env);
    run_nfc_task_once(&env, runtime);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_ERROR);

    h2_runtime_nfc_state_t state;
    assert(h2_runtime_component_state_nfc(runtime, 1u, &state) == H2_PAL_OK);
    assert(state.status == H2_RUNTIME_NFC_STATE_ERROR);
    assert(state.result == H2_PAL_ERR_INVALID_ARG);

    h2_runtime_deinit(runtime);
}

static void test_periph_id_and_unsupported_component_state(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);
    h2_pal_periph_id_t periph_id = 0u;
    assert(h2_runtime_periph_id(runtime, 1u, &periph_id) == H2_PAL_OK);
    assert(periph_id == 10u);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);

    periph_id = 0u;
    assert(h2_runtime_periph_id(runtime, 1u, &periph_id) == H2_PAL_OK);
    assert(periph_id == 10u);

    h2_runtime_component_state_display_t display_state;
    assert(h2_runtime_component_state_display(runtime, &display_state) ==
           H2_PAL_ERR_UNSUPPORTED);

    h2_runtime_deinit(runtime);
}

static void test_runtime_component_registry_queries_physical_kind(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);

    h2_runtime_t *runtime = test_runtime_create(&env);
    h2_runtime_component_info_t info = {0};
    assert(h2_runtime_component_get(runtime, 1u, &info) == H2_PAL_OK);
    assert(info.component_id == 1u);
    assert(info.kind == H2_RUNTIME_COMPONENT_BUTTON);
    assert(h2_runtime_component_get(runtime, 999u, &info) ==
           H2_PAL_ERR_NOT_FOUND);
    h2_runtime_deinit(runtime);
}

static void test_radio_group_is_read_once_for_child_buttons(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    static const h2_pal_periph_radio_button_payload_t radio_payload = {
        .group_id = 40u,
    };
    add_periph(
        &env,
        41u,
        H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
        &radio_payload,
        sizeof(radio_payload));
    add_periph(
        &env,
        42u,
        H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
        &radio_payload,
        sizeof(radio_payload));
    env.button_state.radio_pressed_id = 41u;
    h2_runtime_t *runtime = test_runtime_create(&env);

    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) == H2_PAL_OK);
    assert(state.pressed);
    assert(h2_runtime_component_state_button(runtime, 2u, &state) == H2_PAL_OK);
    assert(!state.pressed);
    assert(env.button_state.group_reads == 1u);
    assert(env.button_state.single_reads == 0u);

    h2_runtime_deinit(runtime);
}

static void test_radio_group_error_advances_child_buttons(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    static const h2_pal_periph_radio_button_payload_t radio_payload = {
        .group_id = 40u,
    };
    add_periph(
        &env,
        41u,
        H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
        &radio_payload,
        sizeof(radio_payload));
    add_periph(
        &env,
        42u,
        H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
        &radio_payload,
        sizeof(radio_payload));
    env.button_state.group_rc = H2_PAL_ERR_IO;
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(env.button_state.group_reads == 1u);

    h2_runtime_deinit(runtime);
}

static void test_runtime_owns_input_task_lifecycle(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    h2_runtime_config_t config = test_runtime_config(&env);
    const h2_runtime_input_poll_config_t input_poll = {
        .tick_ms = 7u,
        .button_poll_interval_ms = 11u,
        .nfc_poll_interval_ms = 13u,
        .imu_poll_interval_ms = 17u,
        .battery_poll_interval_ms = 19u,
        .temperature_poll_interval_ms = 23u,
        .task_options = {
            .name = "runtime-input-test",
            .min_stack_size = 4096u,
        },
    };
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);

    /* Init leaves acquisition stopped; the caller owns the first start. */
    assert(env.task_state.starts == 0u);
    assert(atomic_load(&runtime->private_state->input_phase) ==
           H2_RUNTIME_INPUT_PHASE_STOPPED);
    h2_runtime_button_state_t stopped_state;
    assert(h2_runtime_component_state_button(runtime, 1u, &stopped_state) !=
           H2_PAL_OK);

    assert(h2_runtime_input_start(runtime, &input_poll) == H2_PAL_OK);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(env.task_state.starts == 1u);
    assert(env.task_state.joins == 0u);
    assert(env.time_state.sleep_calls == 0u);
    assert(env.button_state.single_reads == 2u);
    assert(runtime->private_state->input_tick_ms == 7u);
    assert(runtime->private_state->input_button_poll_interval_ms == 11u);
    assert(runtime->private_state->input_nfc_poll_interval_ms == 13u);
    assert(runtime->private_state->input_imu_poll_interval_ms == 17u);
    assert(runtime->private_state->input_battery_poll_interval_ms == 19u);
    assert(runtime->private_state->input_temperature_poll_interval_ms == 23u);
    assert(strcmp(env.task_state.options.name, "runtime-input-test") == 0);
    assert(env.task_state.options.min_stack_size == 4096u);

    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) == H2_PAL_OK);
    assert(state.pressed);
    h2_runtime_deinit(runtime);
    assert(env.task_state.joins == 1u);
}

static void test_input_task_start_failure_leaves_input_stopped(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    env.task_state.start_rc = H2_PAL_ERR_TASK;
    h2_runtime_config_t config = test_runtime_config(&env);
    h2_runtime_t *runtime = NULL;
    /*
     * The Runtime itself no longer depends on the input task, so a task start
     * failure fails the caller's start instead of aborting init. The failed
     * start releases everything it created and stays retryable.
     */
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    assert(runtime != NULL);
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_ERR_TASK);
    assert(env.task_state.current == NULL);
    assert(runtime->private_state->input_task == NULL);
    assert(atomic_load(&runtime->private_state->input_phase) ==
           H2_RUNTIME_INPUT_PHASE_STOPPED);
    /* A failed start only leaves the poller off; init-owned state survives. */
    assert(runtime->private_state->input_writer_mutex != NULL);
    assert(h2_runtime_state_publication_ready(runtime));
    assert(runtime->private_state->input_sources_ready == 1);

    /* A later start succeeds once the task provider recovers. */
    env.task_state.start_rc = H2_PAL_OK;
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
    assert(env.task_state.starts == 1u);

    h2_runtime_deinit(runtime);
    assert(env.task_state.joins == 1u);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_input_worker_failure_closes_event_queue(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(env.task_state.current != NULL);
    env.time_state.sleep_rc = H2_PAL_ERR_IO;
    env.task_state.current->entry(env.task_state.current->ctx);
    assert(atomic_load(&runtime->private_state->input_phase) ==
           H2_RUNTIME_INPUT_PHASE_FAULTED);
    assert(atomic_load(&runtime->private_state->input_worker_result) ==
           H2_PAL_ERR_IO);
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_ERR_CLOSED);
    h2_runtime_deinit(runtime);
    assert(env.task_state.joins == 1u);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_input_join_failure_is_retryable(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);
    env.task_state.join_rc = H2_PAL_ERR_IO;
    h2_runtime_deinit(runtime);
    assert(runtime->private_state->input_task != NULL);
    assert(atomic_load(&runtime->private_state->input_stop_requested) != 0);
    assert(env.task_state.joins == 0u);
    env.task_state.join_rc = H2_PAL_OK;
    h2_runtime_deinit(runtime);
    assert(env.task_state.joins == 1u);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_input_stop_then_start_resumes_acquisition(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(env.task_state.starts == 1u);

    /* One complete click before the stop establishes a click sequence. */
    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    env.time_state.now_ms = 10u;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    env.button_state.single_state = H2_PAL_BUTTON_STATE_RELEASED;
    env.time_state.now_ms = 40u;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    while (h2_runtime_poll_event(runtime, &event) == H2_PAL_OK) {
    }

    assert(h2_runtime_input_stop(runtime) == H2_PAL_OK);
    assert(env.task_state.joins == 1u);
    assert(runtime->private_state->input_task == NULL);
    assert(atomic_load(&runtime->private_state->input_phase) ==
           H2_RUNTIME_INPUT_PHASE_STOPPED);
    /*
     * Stopping the poller only stops the task. The writer mutex, the source
     * table and the published component state all belong to the Runtime and
     * stay exactly as they were, so the last observation is still readable.
     */
    assert(runtime->private_state->input_writer_mutex != NULL);
    assert(runtime->private_state->input_sources_ready == 1);
    assert(h2_runtime_state_publication_ready(runtime));
    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) == H2_PAL_OK);
    assert(!state.pressed);
    assert(state.updated_at_ms == 40u);

    /* Stop is idempotent and does not join a second time. */
    assert(h2_runtime_input_stop(runtime) == H2_PAL_OK);
    assert(env.task_state.joins == 1u);

    const h2_runtime_input_poll_config_t restart_poll = {
        .tick_ms = 5u,
        .button_poll_interval_ms = 9u,
        .task_options = {.name = "runtime-input-restart"},
    };
    assert(h2_runtime_input_start(runtime, &restart_poll) == H2_PAL_OK);
    assert(env.task_state.starts == 2u);
    assert(runtime->private_state->input_task != NULL);
    /* Discovery already ran at init and is not repeated by a restart. */
    assert(env.periphs.list_calls == 1u);
    assert(runtime->private_state->input_tick_ms == 5u);
    assert(runtime->private_state->input_button_poll_interval_ms == 9u);
    assert(strcmp(env.task_state.options.name, "runtime-input-restart") == 0);
    assert(atomic_load(&runtime->private_state->input_phase) ==
           H2_RUNTIME_INPUT_PHASE_TASK_RUNNING);

    /*
     * The first frame after a start republishes the current hardware state.
     * Recognizer state is not reset, because a start switches the poller on
     * and does not touch component state: the click count from before the
     * stop is still the most recent thing the Runtime observed.
     */
    assert(h2_runtime_component_state_button(runtime, 1u, &state) == H2_PAL_OK);
    assert(!state.pressed);
    assert(state.click_count == 1u);

    /* Events flow again; the surviving sequence continues within the gap. */
    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    env.time_state.now_ms = 100u;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    env.button_state.single_state = H2_PAL_BUTTON_STATE_RELEASED;
    env.time_state.now_ms = 130u;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
    h2_runtime_button_action_event_t action;
    memcpy(&action, event.payload, sizeof(action));
    assert(action.click_count == 2u);

    h2_runtime_deinit(runtime);
    assert(env.task_state.joins == 2u);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_input_restart_delivers_queued_push_edges(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    const h2_pal_periph_single_button_payload_t push_payload = {
        .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
    };
    add_periph(&env, 77u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
               &push_payload, sizeof(push_payload));
    h2_runtime_t *runtime = test_runtime_create(&env);

    /* An edge is accepted but not yet consumed when the poller stops. */
    env.time_state.now_ms = 10u;
    assert(h2_runtime_button_push_edge(
               runtime, 77u, H2_RUNTIME_BUTTON_EDGE_DOWN) == H2_PAL_OK);
    assert(h2_runtime_input_stop(runtime) == H2_PAL_OK);
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);

    /*
     * The edge really happened; the poller had simply not consumed it yet.
     * Stopping the poller does not discard queued input, so the restarted
     * poller delivers it instead of dropping it on the floor.
     */
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) == H2_PAL_OK);
    assert(state.pressed);

    h2_runtime_deinit(runtime);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_input_start_without_mapped_input_is_noop(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(env.task_state.starts == 0u);
    assert(atomic_load(&runtime->private_state->input_phase) ==
           H2_RUNTIME_INPUT_PHASE_STOPPED);

    assert(h2_runtime_input_stop(runtime) == H2_PAL_OK);
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
    assert(env.task_state.starts == 0u);
    assert(env.task_state.joins == 0u);
    assert(runtime->private_state->input_task == NULL);
    assert(atomic_load(&runtime->private_state->input_phase) ==
           H2_RUNTIME_INPUT_PHASE_STOPPED);

    h2_runtime_deinit(runtime);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_input_double_start_is_rejected(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(env.task_state.starts == 1u);
    h2_pal_mutex_t *mutex = runtime->private_state->input_writer_mutex;

    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_ERR_INVALID_STATE);
    assert(env.task_state.starts == 1u);
    assert(runtime->private_state->input_writer_mutex == mutex);
    assert(atomic_load(&runtime->private_state->input_phase) ==
           H2_RUNTIME_INPUT_PHASE_TASK_RUNNING);

    h2_runtime_deinit(runtime);
    assert(env.task_state.joins == 1u);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_input_start_after_worker_fault_is_rejected(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(env.task_state.current != NULL);

    env.time_state.sleep_rc = H2_PAL_ERR_IO;
    env.task_state.current->entry(env.task_state.current->ctx);
    assert(atomic_load(&runtime->private_state->input_phase) ==
           H2_RUNTIME_INPUT_PHASE_FAULTED);

    /* Stop reports the fault; the closed event queue makes it terminal. */
    env.time_state.sleep_rc = H2_PAL_OK;
    assert(h2_runtime_input_stop(runtime) == H2_PAL_ERR_IO);
    assert(atomic_load(&runtime->private_state->input_phase) ==
           H2_RUNTIME_INPUT_PHASE_STOPPED);
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_ERR_INVALID_STATE);
    assert(env.task_state.starts == 1u);
    assert(runtime->private_state->input_task == NULL);

    h2_runtime_deinit(runtime);
    assert(env.task_state.joins == 1u);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_input_lifecycle_is_closed_during_test_session(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);

    h2_runtime_test_control_t *control = NULL;
    assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_OK);
    /*
     * The session owns the source table, so starting the poller underneath it
     * is refused. Stopping is harmless now that stop only stops the task, so
     * the two are deliberately not symmetric.
     */
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_ERR_INVALID_STATE);
    assert(env.task_state.starts == 1u);
    assert(h2_runtime_input_stop(runtime) == H2_PAL_OK);
    assert(env.task_state.joins == 1u);
    assert(h2_runtime_state_publication_ready(runtime));
    h2_runtime_test_control_close(control);

    /* A session may also open and close while the poller is stopped. */
    assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_OK);
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_ERR_INVALID_STATE);
    h2_runtime_test_control_close(control);

    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
    assert(env.task_state.starts == 2u);
    env.button_state.single_state = H2_PAL_BUTTON_STATE_PRESSED;
    env.time_state.now_ms = 200u;
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);

    h2_runtime_deinit(runtime);
    assert(env.task_state.joins == 2u);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_input_snapshot_does_not_create_condition(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    env.sync_state.cond_create_rc = H2_PAL_ERR_IO;
    h2_runtime_config_t config = test_runtime_config(&env);
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    assert(runtime != NULL);
    /* Only the input writer mutex is created; no condition variable. */
    assert(env.sync_state.creates == 1u);
    assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
    assert(env.sync_state.creates == 1u);
    h2_runtime_deinit(runtime);
    assert(env.sync_state.destroys == 1u);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_sequence_continues_past_uint32_max(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_t *runtime = test_runtime_create(&env);
    runtime->private_state->next_sequence = (h2_runtime_sequence_t)UINT32_MAX;

    assert(h2_runtime_next_sequence(runtime) == (h2_runtime_sequence_t)UINT32_MAX);
    assert(h2_runtime_next_sequence(runtime) == (h2_runtime_sequence_t)UINT32_MAX + 1u);

    h2_runtime_deinit(runtime);
}

static void test_sensor_component_mapping(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 401u, H2_PAL_PERIPH_TYPE_BATTERY, NULL, 0u);
    add_periph(&env, 501u, H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR, NULL, 0u);
    add_periph(&env, 701u, H2_PAL_PERIPH_TYPE_PWM_SWITCH, NULL, 0u);
    add_periph(&env, 801u, H2_PAL_PERIPH_TYPE_BUZZER, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);

    assert(runtime->private_state->component_mapping_count == 4u);
    const h2_runtime_component_mapping_t *battery =
        h2_runtime_find_component_mapping_by_periph(runtime, 401u);
    const h2_runtime_component_mapping_t *temperature =
        h2_runtime_find_component_mapping_by_periph(runtime, 501u);
    const h2_runtime_component_mapping_t *pwm =
        h2_runtime_find_component_mapping_by_periph(runtime, 701u);
    const h2_runtime_component_mapping_t *buzzer =
        h2_runtime_find_component_mapping_by_periph(runtime, 801u);
    assert(battery != NULL && battery->component == H2_RUNTIME_COMPONENT_BATTERY);
    assert(temperature != NULL &&
           temperature->component == H2_RUNTIME_COMPONENT_TEMPERATURE_SENSOR);
    assert(pwm != NULL && pwm->component == H2_RUNTIME_COMPONENT_PWM_SWITCH);
    assert(buzzer != NULL && buzzer->component == H2_RUNTIME_COMPONENT_BUZZER);

    assert(runtime->private_state->input_source_count == 2u);
    assert(env.task_state.starts == 1u);
    assert(env.input_state.battery_reads == 1u);
    assert(env.input_state.temperature_reads == 1u);
    h2_runtime_battery_state_t battery_state = {0};
    assert(h2_runtime_component_state_battery(runtime, battery->component_id,
                                              &battery_state) == H2_PAL_OK);
    assert(battery_state.result == H2_PAL_OK);
    assert(battery_state.reading.id == 401u);
    h2_runtime_temperature_state_t temperature_state = {0};
    assert(h2_runtime_component_state_temperature(
               runtime, temperature->component_id, &temperature_state) ==
           H2_PAL_OK);
    assert(temperature_state.result == H2_PAL_OK);
    assert(temperature_state.reading.id == 501u);
    h2_runtime_deinit(runtime);
}

static void test_control_keeps_test_sources_while_sensors_poll(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    add_periph(&env, 401u, H2_PAL_PERIPH_TYPE_BATTERY, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);
    const h2_runtime_component_mapping_t *button =
        h2_runtime_find_component_mapping_by_periph(runtime, 10u);
    const h2_runtime_component_mapping_t *battery =
        h2_runtime_find_component_mapping_by_periph(runtime, 401u);
    assert(button != NULL && battery != NULL);
    const size_t battery_reads_before = env.input_state.battery_reads;
    const size_t button_reads_before = env.button_state.single_reads;

    h2_runtime_test_control_t *control = NULL;
    assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_OK);
    assert(h2_runtime_test_button_down(
               control, button->component_id, 100u) == H2_PAL_OK);

    /* A sensor-only poll refreshes the passive sensors from their PAL ... */
    env.time_state.now_ms = 600u;
    assert(h2_runtime_test_poll_sensors(runtime) == H2_PAL_OK);
    assert(env.input_state.battery_reads == battery_reads_before + 1u);
    h2_runtime_battery_state_t battery_state = {0};
    assert(h2_runtime_component_state_battery(
               runtime, battery->component_id, &battery_state) == H2_PAL_OK);
    assert(battery_state.result == H2_PAL_OK);
    assert(battery_state.reading.id == 401u);

    /* ... without rediscovering physical sources over the test-owned table. */
    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(
               runtime, button->component_id, &state) == H2_PAL_OK);
    assert(state.pressed);
    assert(state.pressed_at_ms == 100u);
    assert(env.button_state.single_reads == button_reads_before);

    h2_runtime_test_control_close(control);
    h2_runtime_deinit(runtime);
}

static void test_control_button_down_tracks_click_sequence(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(&env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);
    h2_runtime_test_control_t *control = NULL;
    assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_OK);

    h2_runtime_button_state_t state;
    const size_t button_locks_before = env.sync_state.locks;
    const size_t button_unlocks_before = env.sync_state.unlocks;
    assert(h2_runtime_test_button_down(control, 1u, 100u) == H2_PAL_OK);
    assert(env.sync_state.locks == button_locks_before + 1u);
    assert(env.sync_state.unlocks == button_unlocks_before + 1u);
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(state.pressed && state.click_count == 1u);

    /* Release, then press again within the click gap: count continues. */
    assert(h2_runtime_test_button_up(control, 1u, 100u, 140u) == H2_PAL_OK);
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(!state.pressed && state.click_count == 1u);
    assert(h2_runtime_test_button_down(
               control, 1u, 140u + H2_RUNTIME_BUTTON_CLICK_GAP_MS) ==
           H2_PAL_OK);
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(state.pressed && state.click_count == 2u);

    /* An action carries its own count; a press after the gap restarts. */
    assert(h2_runtime_test_button_action(
               control, 1u, 390u, 420u, 3u) == H2_PAL_OK);
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(!state.pressed && state.click_count == 3u);
    assert(h2_runtime_test_button_down(
               control, 1u, 420u + H2_RUNTIME_BUTTON_CLICK_GAP_MS + 1u) ==
           H2_PAL_OK);
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(state.pressed && state.click_count == 1u);

    h2_runtime_test_control_close(control);
    h2_runtime_deinit(runtime);
}

static void test_control_injects_validated_runtime_events(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_t *runtime = test_runtime_create(&env);
    h2_runtime_test_control_t *control = NULL;

    assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_OK);
    assert(control != NULL);
    h2_runtime_test_control_t *duplicate = NULL;
    assert(h2_runtime_test_control_open(runtime, &duplicate) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(duplicate == NULL);

    h2_runtime_system_event_wifi_sta_t wifi;
    memset(&wifi, 0, sizeof(wifi));
    wifi.status = H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_CONNECTED;
    assert(h2_runtime_test_emit_event(
               control,
               H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED,
               H2_RUNTIME_COMPONENT_SYSTEM_WIFI,
               H2_RUNTIME_COMPONENT_ID_NONE,
               42u,
               &wifi,
               sizeof(wifi)) == H2_PAL_OK);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED);
    assert(event.component == H2_RUNTIME_COMPONENT_SYSTEM_WIFI);
    assert(event.sequence == 1u);
    assert(event.timestamp_ms == 42u);
    assert(event.payload_size == sizeof(wifi));
    assert(memcmp(event.payload, &wifi, sizeof(wifi)) == 0);

    assert(h2_runtime_test_emit_event(
               control,
               H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED,
               H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
               H2_RUNTIME_COMPONENT_ID_NONE,
               43u,
               &wifi,
               sizeof(wifi)) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_runtime_test_emit_event(
               control,
               H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED,
               H2_RUNTIME_COMPONENT_SYSTEM_WIFI,
               H2_RUNTIME_COMPONENT_ID_NONE,
               43u,
               &wifi,
               sizeof(wifi) - 1u) == H2_PAL_ERR_INVALID_ARG);
    assert(runtime->private_state->next_sequence == 2u);

    h2_runtime_test_control_close(control);
    assert(runtime->private_state->test_control == NULL);
    assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_OK);
    h2_runtime_test_control_close(control);
    h2_runtime_deinit(runtime);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_control_open_failure_is_transactional(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_t *runtime = test_runtime_create(&env);
    h2_runtime_test_control_t *control = NULL;

    env.queue_state.reset_rc = H2_PAL_ERR_IO;
    assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_ERR_IO);
    assert(control == NULL);
    assert(runtime->private_state->test_control == NULL);

    env.queue_state.reset_rc = H2_PAL_OK;
    assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_OK);
    h2_runtime_test_control_close(control);
    h2_runtime_deinit(runtime);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_control_button_helpers_share_state_and_event_sequence(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    add_periph(
        &env, 10u, H2_PAL_PERIPH_TYPE_SINGLE_BUTTON, NULL, 0u);
    h2_runtime_t *runtime = test_runtime_create(&env);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);
    h2_runtime_test_control_t *control = NULL;
    assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_OK);
    assert(h2_runtime_input_poll_once(runtime) == H2_PAL_OK);

    assert(h2_runtime_test_button_down(control, 1u, 100u) == H2_PAL_OK);
    h2_runtime_button_state_t state;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(state.pressed);
    assert(state.pressed_at_ms == 100u);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN);
    assert(event.sequence ==
           runtime->private_state->input_sources[0].sequence);
    const unsigned int active_index = atomic_load_explicit(
        &runtime->private_state->state_publication.active_index,
        memory_order_acquire);
    assert(runtime->private_state->state_publication.banks[active_index]
               .event_sequence_ceiling >= event.sequence);
    assert(event.payload_size == sizeof(h2_runtime_button_down_event_t));

    assert(h2_runtime_test_button_action(control, 1u, 100u, 140u, 1u) ==
           H2_PAL_OK);
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(!state.pressed);
    assert(state.updated_at_ms == 140u);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION);
    assert(event.sequence ==
           runtime->private_state->input_sources[0].sequence);

    const h2_runtime_button_state_t prior_state = state;
    const h2_runtime_sequence_t prior_sequence =
        runtime->private_state->input_sources[0].sequence;
    const h2_runtime_sequence_t prior_ceiling =
        runtime->private_state->input_event_sequence_ceiling;
    env.queue_state.send_rc = H2_PAL_QUEUE_ERR_IO;
    assert(h2_runtime_test_button_up(control, 1u, 100u, 160u) ==
           H2_PAL_QUEUE_ERR_IO);
    env.queue_state.send_rc = H2_PAL_OK;
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_OK);
    assert(memcmp(&state, &prior_state, sizeof(state)) == 0);
    assert(runtime->private_state->input_sources[0].sequence ==
           prior_sequence);
    assert(runtime->private_state->input_event_sequence_ceiling ==
           prior_ceiling);
    assert(h2_runtime_poll_event(runtime, &event) ==
           H2_PAL_ERR_WOULD_BLOCK);

    const h2_runtime_sequence_t next_sequence =
        runtime->private_state->next_sequence;
    state.updated_at_ms = 150u;
    const size_t state_locks_before = env.sync_state.locks;
    const size_t state_unlocks_before = env.sync_state.unlocks;
    assert(h2_runtime_test_set_component_state(
               control, 1u, &state, sizeof(state)) == H2_PAL_OK);
    assert(env.sync_state.locks == state_locks_before + 1u);
    assert(env.sync_state.unlocks == state_unlocks_before + 1u);
    assert(runtime->private_state->next_sequence == next_sequence);
    h2_runtime_button_state_t published_state;
    assert(h2_runtime_component_state_button(
               runtime, 1u, &published_state) == H2_PAL_OK);
    assert(published_state.updated_at_ms == 150u);

    h2_runtime_button_state_t invalid = state;
    invalid.pressed_at_ms = 1u;
    assert(h2_runtime_test_set_component_state(
               control, 1u, &invalid, sizeof(invalid)) == H2_PAL_ERR_FORMAT);
    assert(h2_runtime_test_button_up(control, 1u, 200u, 199u) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_runtime_test_set_component_state(
               control, 999u, &state, sizeof(state)) ==
           H2_PAL_ERR_NOT_FOUND);
    assert(h2_runtime_component_state_button(runtime, 999u, &state) ==
           H2_PAL_ERR_NOT_FOUND);

    h2_runtime_test_control_close(control);
    assert(h2_runtime_component_state_button(runtime, 1u, &state) ==
           H2_PAL_ERR_NOT_FOUND);
    h2_runtime_deinit(runtime);
    assert(env.allocator_state.alloc_calls == env.allocator_state.free_calls);
}

static void test_control_preserves_runtime_queue_drop_behavior(void) {
    test_runtime_env_t env;
    test_env_init(&env);
    h2_runtime_config_t config = test_runtime_config(&env);
    config.event_queue_capacity = 1u;
    h2_runtime_t *runtime = NULL;
    assert(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
    h2_runtime_test_control_t *control = NULL;
    assert(h2_runtime_test_control_open(runtime, &control) == H2_PAL_OK);

    assert(h2_runtime_test_emit_event(
               control,
               H2_RUNTIME_SYSTEM_EVENT_MODEM_READY,
               H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
               H2_RUNTIME_COMPONENT_ID_NONE,
               10u,
               NULL,
               0u) == H2_PAL_OK);
    assert(h2_runtime_test_emit_event(
               control,
               H2_RUNTIME_SYSTEM_EVENT_MODEM_READY,
               H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
               H2_RUNTIME_COMPONENT_ID_NONE,
               11u,
               NULL,
               0u) == H2_PAL_OK);
    assert(runtime->private_state->dropped_event_count == 1u);
    assert(runtime->private_state->next_sequence == 3u);

    uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    h2_runtime_event_t event = event_with_payload(payload);
    assert(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    assert(event.timestamp_ms == 10u);
    assert(h2_runtime_poll_event(runtime, &event) ==
           H2_PAL_ERR_WOULD_BLOCK);

    h2_runtime_test_control_close(control);
    h2_runtime_deinit(runtime);
}

int main(void) {
    test_runtime_firmware_info_provider();
    test_runtime_capabilities_are_bound_at_init();
    test_runtime_rejects_incomplete_video_decoder();
    test_runtime_rejects_missing_wifi_csi();
    test_runtime_rejects_missing_nfc_card_emulation();
    test_runtime_buzzer_contract();
    test_runtime_binds_unsupported_nfc_card_emulation();
    test_runtime_private_allocation_failure_cleans_up();
    test_runtime_uses_initialization_capacities();
    test_runtime_rejects_invalid_initialization_capacities();
    test_runtime_allocation_failures_release_owned_memory();
    test_runtime_mapping_queries_only_mapped_peripherals();
    test_runtime_reports_configured_capacity_exhaustion();
    test_runtime_payload_capacity_only_limits_enqueue();
    test_system_event_lifecycle_and_gpio_payload_copy();
    test_system_event_maximum_ble_value_is_copied();
    test_system_event_netif_transitions_copy_and_order();
    test_system_event_partial_subscribe_cleans_up();
    test_system_event_projects_all_scope_events();
    test_system_event_modem_call_number_is_terminated();
    test_system_event_rejects_invalid_payloads();
    test_system_event_advertising_set_correlation_and_copy();
    test_system_event_advertising_queue_failure();
    test_event_small_buffer_does_not_dequeue();
    test_event_queue_timeout_drops_event();
    test_button_action_emits_on_release();
    test_button_rapid_clicks_emit_separate_events();
    test_button_held_polls_stay_silent_and_release_emits_action();
    test_push_button_uses_mapping_and_runtime_gesture_recognizer();
    test_test_control_discards_stale_push_edges();
    test_button_push_rejects_non_push_and_invalid_payload();
    test_nfc_discovery_and_state();
    test_nfc_background_task_does_not_block_input_task();
    test_imu_tilt_event_and_state();
    test_imu_shake_suppresses_intermediate_tilt();
    test_imu_int32_min_sample_is_safe();
    test_imu_ignores_samples_without_required_flags();
    test_nfc_rejects_oversized_uid_len();
    test_periph_id_and_unsupported_component_state();
    test_runtime_component_registry_queries_physical_kind();
    test_radio_group_is_read_once_for_child_buttons();
    test_radio_group_error_advances_child_buttons();
    test_runtime_owns_input_task_lifecycle();
    test_input_task_start_failure_leaves_input_stopped();
    test_input_worker_failure_closes_event_queue();
    test_input_join_failure_is_retryable();
    test_input_stop_then_start_resumes_acquisition();
    test_input_restart_delivers_queued_push_edges();
    test_input_start_without_mapped_input_is_noop();
    test_input_double_start_is_rejected();
    test_input_start_after_worker_fault_is_rejected();
    test_input_lifecycle_is_closed_during_test_session();
    test_input_snapshot_does_not_create_condition();
    test_sequence_continues_past_uint32_max();
    test_sensor_component_mapping();
    test_control_keeps_test_sources_while_sensors_poll();
    test_control_button_down_tracks_click_sequence();
    test_control_injects_validated_runtime_events();
    test_control_open_failure_is_transactional();
    test_control_button_helpers_share_state_and_event_sequence();
    test_control_preserves_runtime_queue_drop_behavior();
    return 0;
}
