#include "h2_lvgl_platform.h"
#include "h2_lvgl_task_names.h"

#include "lvgl.h"
#include "osal/lv_os_private.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct h2_lvgl_platform_state {
    const h2_pal_mem_api_t *allocator;
    const h2_pal_task_api_t *task_api;
    const h2_pal_sync_api_t *sync_api;
    const h2_pal_queue_api_t *queue_api;
    const h2_pal_time_api_t *time_api;
    int initialized;
} h2_lvgl_platform_state_t;

struct h2_lvgl_thread {
    h2_pal_task_t *task;
    void (*callback)(void *);
    void *user_data;
};

struct h2_lvgl_mutex {
    h2_pal_mutex_t *mutex;
};

struct h2_lvgl_thread_sync {
    h2_pal_queue_t *queue;
};

static h2_lvgl_platform_state_t s_h2_lvgl_platform;

static int platform_ready(void) {
    return s_h2_lvgl_platform.initialized &&
        s_h2_lvgl_platform.allocator != NULL &&
        s_h2_lvgl_platform.task_api != NULL &&
        s_h2_lvgl_platform.sync_api != NULL &&
        s_h2_lvgl_platform.queue_api != NULL &&
        s_h2_lvgl_platform.time_api != NULL;
}

static void *lvgl_alloc(size_t len) {
    if (!platform_ready()) {
        return NULL;
    }
    return h2_pal_mem_alloc(s_h2_lvgl_platform.allocator, len);
}

static void lvgl_free(void *ptr) {
    if (!platform_ready() || ptr == NULL) {
        return;
    }
    h2_pal_mem_free(s_h2_lvgl_platform.allocator, ptr);
}

void lv_mem_init(void) {
}

void lv_mem_deinit(void) {
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    (void)pool;
}

void *lv_malloc_core(size_t size) {
    return lvgl_alloc(size);
}

void *lv_realloc_core(void *ptr, size_t new_size) {
    if (!platform_ready()) {
        return NULL;
    }
    if (ptr == NULL) {
        return lvgl_alloc(new_size);
    }
    return h2_pal_mem_realloc(s_h2_lvgl_platform.allocator, ptr, new_size);
}

void lv_free_core(void *ptr) {
    lvgl_free(ptr);
}

void lv_mem_monitor_core(lv_mem_monitor_t *monitor) {
    if (monitor != NULL) {
        memset(monitor, 0, sizeof(*monitor));
    }
}

lv_result_t lv_mem_test_core(void) {
    void *memory = lvgl_alloc(1u);
    if (memory == NULL) {
        return LV_RESULT_INVALID;
    }
    lvgl_free(memory);
    return LV_RESULT_OK;
}

static void h2_lvgl_thread_entry(void *ctx) {
    struct h2_lvgl_thread *thread = (struct h2_lvgl_thread *)ctx;
    if (thread != NULL && thread->callback != NULL) {
        thread->callback(thread->user_data);
    }
}

static const char *h2_lvgl_task_name(const char *name) {
    if (name == NULL) return NULL;
    if (strcmp(name, "swdraw") == 0) return h2_lvgl_software_draw_task_name;
    if (strcmp(name, "g2ddraw") == 0) return h2_lvgl_g2d_draw_task_name;
    if (strcmp(name, "pxpdraw") == 0) return h2_lvgl_pxp_draw_task_name;
    if (strcmp(name, "nemagfx") == 0) return h2_lvgl_nema_gfx_task_name;
    return NULL;
}

int h2_lvgl_platform_init(const h2_lvgl_platform_config_t *config) {
    if (config == NULL ||
        config->allocator == NULL ||
        config->task_api == NULL ||
        config->sync_api == NULL ||
        config->queue_api == NULL ||
        config->time_api == NULL) {
        return -1;
    }

    s_h2_lvgl_platform.allocator = config->allocator;
    s_h2_lvgl_platform.task_api = config->task_api;
    s_h2_lvgl_platform.sync_api = config->sync_api;
    s_h2_lvgl_platform.queue_api = config->queue_api;
    s_h2_lvgl_platform.time_api = config->time_api;
    s_h2_lvgl_platform.initialized = 1;
    return 0;
}

void h2_lvgl_platform_deinit(void) {
    memset(&s_h2_lvgl_platform, 0, sizeof(s_h2_lvgl_platform));
}

lv_result_t lv_thread_init(
    lv_thread_t *thread,
    const char *const name,
    lv_thread_prio_t prio,
    void (*callback)(void *),
    size_t stack_size,
    void *user_data) {
    (void)prio;

    if (thread == NULL || callback == NULL || !platform_ready()) {
        return LV_RESULT_INVALID;
    }
    *thread = NULL;

    struct h2_lvgl_thread *state = (struct h2_lvgl_thread *)lvgl_alloc(sizeof(*state));
    if (state == NULL) {
        return LV_RESULT_INVALID;
    }
    memset(state, 0, sizeof(*state));
    state->callback = callback;
    state->user_data = user_data;

    const char *portable_name = h2_lvgl_task_name(name);
    if (portable_name == NULL) {
        lvgl_free(state);
        return LV_RESULT_INVALID;
    }
    h2_pal_task_options_t options = {
        .name = portable_name,
        .min_stack_size = stack_size,
    };
    int rc = h2_pal_task_start(
        s_h2_lvgl_platform.task_api,
        &options,
        h2_lvgl_thread_entry,
        state,
        &state->task);
    if (rc != 0) {
        lvgl_free(state);
        return LV_RESULT_INVALID;
    }

    *thread = state;
    return LV_RESULT_OK;
}

lv_result_t lv_thread_delete(lv_thread_t *thread) {
    if (thread == NULL || *thread == NULL || !platform_ready()) {
        return LV_RESULT_INVALID;
    }

    struct h2_lvgl_thread *state = *thread;
    int rc = h2_pal_task_join(s_h2_lvgl_platform.task_api, state->task);
    if (rc != 0) {
        return LV_RESULT_INVALID;
    }

    lvgl_free(state);
    *thread = NULL;
    return LV_RESULT_OK;
}

lv_result_t lv_mutex_init(lv_mutex_t *mutex) {
    if (mutex == NULL || !platform_ready()) {
        return LV_RESULT_INVALID;
    }
    *mutex = NULL;

    struct h2_lvgl_mutex *state = (struct h2_lvgl_mutex *)lvgl_alloc(sizeof(*state));
    if (state == NULL) {
        return LV_RESULT_INVALID;
    }
    memset(state, 0, sizeof(*state));

    h2_pal_mutex_config_t config = {
        .name = "lvgl",
        .allocator = s_h2_lvgl_platform.allocator,
        .flags = H2_PAL_MUTEX_FLAG_RECURSIVE,
    };
    h2_pal_result_t rc = h2_pal_mutex_create(
        s_h2_lvgl_platform.sync_api,
        &config,
        &state->mutex);
    if (rc != H2_PAL_OK) {
        lvgl_free(state);
        return LV_RESULT_INVALID;
    }

    *mutex = state;
    return LV_RESULT_OK;
}

lv_result_t lv_mutex_lock(lv_mutex_t *mutex) {
    if (mutex == NULL || *mutex == NULL || !platform_ready()) {
        return LV_RESULT_INVALID;
    }
    h2_pal_result_t rc = h2_pal_mutex_lock(
        s_h2_lvgl_platform.sync_api,
        (*mutex)->mutex);
    return rc == H2_PAL_OK ? LV_RESULT_OK : LV_RESULT_INVALID;
}

lv_result_t lv_mutex_lock_isr(lv_mutex_t *mutex) {
    return lv_mutex_lock(mutex);
}

lv_result_t lv_mutex_unlock(lv_mutex_t *mutex) {
    if (mutex == NULL || *mutex == NULL || !platform_ready()) {
        return LV_RESULT_INVALID;
    }
    h2_pal_result_t rc = h2_pal_mutex_unlock(
        s_h2_lvgl_platform.sync_api,
        (*mutex)->mutex);
    return rc == H2_PAL_OK ? LV_RESULT_OK : LV_RESULT_INVALID;
}

lv_result_t lv_mutex_delete(lv_mutex_t *mutex) {
    if (mutex == NULL || *mutex == NULL || !platform_ready()) {
        return LV_RESULT_INVALID;
    }

    (void)h2_pal_mutex_destroy(
        s_h2_lvgl_platform.sync_api,
        (*mutex)->mutex);
    lvgl_free(*mutex);
    *mutex = NULL;
    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_init(lv_thread_sync_t *sync) {
    if (sync == NULL || !platform_ready()) {
        return LV_RESULT_INVALID;
    }
    *sync = NULL;

    struct h2_lvgl_thread_sync *state = (struct h2_lvgl_thread_sync *)lvgl_alloc(sizeof(*state));
    if (state == NULL) {
        return LV_RESULT_INVALID;
    }
    memset(state, 0, sizeof(*state));

    h2_pal_queue_config_t config = {
        .name = "lvgl/sync",
        .item_size = sizeof(uint8_t),
        .item_count = 1,
        .allocator = s_h2_lvgl_platform.allocator,
    };
    int rc = h2_pal_queue_create(
        s_h2_lvgl_platform.queue_api,
        &config,
        &state->queue);
    if (rc != H2_PAL_QUEUE_OK) {
        lvgl_free(state);
        return LV_RESULT_INVALID;
    }

    *sync = state;
    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_wait(lv_thread_sync_t *sync) {
    if (sync == NULL || *sync == NULL || !platform_ready()) {
        return LV_RESULT_INVALID;
    }

    uint8_t token = 0;
    int rc = h2_pal_queue_recv(
        s_h2_lvgl_platform.queue_api,
        (*sync)->queue,
        &token,
        H2_PAL_QUEUE_WAIT_FOREVER);
    return rc == H2_PAL_QUEUE_OK ? LV_RESULT_OK : LV_RESULT_INVALID;
}

lv_result_t lv_thread_sync_signal(lv_thread_sync_t *sync) {
    if (sync == NULL || *sync == NULL || !platform_ready()) {
        return LV_RESULT_INVALID;
    }

    uint8_t token = 1;
    int rc = h2_pal_queue_send(
        s_h2_lvgl_platform.queue_api,
        (*sync)->queue,
        &token,
        H2_PAL_QUEUE_NO_WAIT);
    if (rc == H2_PAL_QUEUE_OK || rc == H2_PAL_QUEUE_ERR_TIMEOUT) {
        return LV_RESULT_OK;
    }
    return LV_RESULT_INVALID;
}

lv_result_t lv_thread_sync_signal_isr(lv_thread_sync_t *sync) {
    return lv_thread_sync_signal(sync);
}

lv_result_t lv_thread_sync_delete(lv_thread_sync_t *sync) {
    if (sync == NULL || *sync == NULL || !platform_ready()) {
        return LV_RESULT_INVALID;
    }

    h2_pal_queue_destroy(s_h2_lvgl_platform.queue_api, (*sync)->queue);
    lvgl_free(*sync);
    *sync = NULL;
    return LV_RESULT_OK;
}

uint32_t lv_os_get_idle_percent(void) {
    return lv_timer_get_idle();
}

void lv_sleep_ms(uint32_t ms) {
    if (platform_ready()) {
        (void)h2_pal_time_sleep_ms(s_h2_lvgl_platform.time_api, ms);
    }
}
