#include "h2_modem_urc.h"

#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

static void counter_increment(uint32_t *counter) {
#if defined(_MSC_VER) && !defined(__clang__)
    (void)_InterlockedIncrement((volatile long *)counter);
#else
    (void)__atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED);
#endif
}

static uint32_t counter_load(const uint32_t *counter) {
#if defined(_MSC_VER) && !defined(__clang__)
    return (uint32_t)_InterlockedCompareExchange((volatile long *)counter, 0, 0);
#else
    return __atomic_load_n(counter, __ATOMIC_RELAXED);
#endif
}

typedef struct urc_line {
    char text[H2_MODEM_URC_LINE_MAX];
} urc_line_t;

static void urc_task(void *user) {
    h2_modem_urc_worker_t *worker = user;
    for (;;) {
        urc_line_t line;
        h2_pal_result_t rc = (h2_pal_result_t)h2_pal_queue_recv(
            worker->queue_api, worker->queue, &line, H2_PAL_QUEUE_WAIT_FOREVER);
        if (rc != H2_PAL_OK) {
            worker->result = rc == H2_PAL_ERR_CLOSED ? H2_PAL_OK : rc;
            return;
        }
        worker->handler(worker->user, line.text);
        counter_increment(&worker->stats.handled);
    }
}

h2_pal_result_t h2_modem_urc_start(
    h2_modem_urc_worker_t *worker,
    const h2_pal_task_api_t *task_api,
    const h2_pal_queue_api_t *queue_api,
    const h2_pal_mem_api_t *allocator,
    h2_modem_urc_handler_t handler,
    void *user) {
    if (worker == NULL || handler == NULL || task_api == NULL ||
        task_api->vtable == NULL || task_api->vtable->start == NULL ||
        task_api->vtable->join == NULL || queue_api == NULL ||
        queue_api->vtable == NULL || queue_api->vtable->create == NULL ||
        queue_api->vtable->destroy == NULL || queue_api->vtable->send == NULL ||
        queue_api->vtable->recv == NULL || queue_api->vtable->close == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (worker->task != NULL || worker->queue != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *worker = (h2_modem_urc_worker_t){
        .task_api = task_api, .queue_api = queue_api, .handler = handler, .user = user,
    };
    h2_pal_queue_config_t queue_config = {
        .name = H2_MODEM_URC_TASK_NAME,
        .item_size = sizeof(urc_line_t),
        .item_count = H2_MODEM_URC_QUEUE_SIZE,
        .allocator = allocator,
    };
    h2_pal_result_t rc = (h2_pal_result_t)h2_pal_queue_create(queue_api, &queue_config, &worker->queue);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_pal_task_options_t options = {.name = H2_MODEM_URC_TASK_NAME, .min_stack_size = 4096u};
    rc = h2_pal_task_start(task_api, &options, urc_task, worker, &worker->task);
    if (rc != H2_PAL_OK) {
        h2_pal_queue_destroy(queue_api, worker->queue);
        worker->queue = NULL;
    }
    return rc;
}

h2_pal_result_t h2_modem_urc_post(h2_modem_urc_worker_t *worker, const char *line) {
    if (worker == NULL || line == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (worker->queue == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    size_t len = 0u;
    while (len < H2_MODEM_URC_LINE_MAX && line[len] != '\0') { len++; }
    if (len == H2_MODEM_URC_LINE_MAX) {
        counter_increment(&worker->stats.truncated);
        return H2_PAL_ERR_TRUNCATED;
    }
    if (len == 0u) {
        return H2_PAL_OK;
    }
    urc_line_t item = {0};
    memcpy(item.text, line, len);
    h2_pal_result_t rc = (h2_pal_result_t)h2_pal_queue_send(
        worker->queue_api, worker->queue, &item, H2_PAL_QUEUE_NO_WAIT);
    if (rc == H2_PAL_OK) {
        counter_increment(&worker->stats.accepted);
    } else if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_FULL) {
        counter_increment(&worker->stats.full);
        rc = H2_PAL_ERR_FULL;
    }
    return rc;
}

h2_pal_result_t h2_modem_urc_stop(h2_modem_urc_worker_t *worker) {
    if (worker == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (worker->task == NULL) {
        return H2_PAL_OK;
    }
    if (!worker->stopping) {
        h2_pal_result_t rc = (h2_pal_result_t)h2_pal_queue_close(worker->queue_api, worker->queue);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        worker->stopping = 1;
    }
    h2_pal_result_t rc = h2_pal_task_join(worker->task_api, worker->task);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    worker->task = NULL;
    h2_pal_queue_destroy(worker->queue_api, worker->queue);
    worker->queue = NULL;
    return worker->result;
}

h2_pal_result_t h2_modem_urc_get_stats(
    const h2_modem_urc_worker_t *worker, h2_modem_urc_stats_t *out_stats) {
    if (worker == NULL || out_stats == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_stats->accepted = counter_load(&worker->stats.accepted);
    out_stats->handled = counter_load(&worker->stats.handled);
    out_stats->full = counter_load(&worker->stats.full);
    out_stats->truncated = counter_load(&worker->stats.truncated);
    return H2_PAL_OK;
}
