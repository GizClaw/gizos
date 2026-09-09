#ifndef H2_MODEM_URC_H
#define H2_MODEM_URC_H

#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_MODEM_URC_TASK_NAME "$modem/urc"
#define H2_MODEM_URC_LINE_MAX 192u
#define H2_MODEM_URC_QUEUE_SIZE 16u

typedef void (*h2_modem_urc_handler_t)(void *user, const char *line);

typedef struct h2_modem_urc_worker {
    const h2_pal_task_api_t *task_api;
    const h2_pal_queue_api_t *queue_api;
    h2_pal_task_t *task;
    h2_pal_queue_t *queue;
    h2_modem_urc_handler_t handler;
    void *user;
    h2_pal_result_t result;
    int stopping;
} h2_modem_urc_worker_t;

/* Zero-initialize before first start. Lifecycle calls are externally
 * serialized. Start before enabling RX. */
h2_pal_result_t h2_modem_urc_start(
    h2_modem_urc_worker_t *worker,
    const h2_pal_task_api_t *task_api,
    const h2_pal_queue_api_t *queue_api,
    const h2_pal_mem_api_t *allocator,
    h2_modem_urc_handler_t handler,
    void *user);

/* Copy a complete, NUL-terminated line from task context. Never waits for
 * queue space or handler completion. FULL/TRUNCATED must be handled by the
 * transport; notifications are never overwritten or processed inline. */
h2_pal_result_t h2_modem_urc_post(h2_modem_urc_worker_t *worker, const char *line);

/* Stop/join RX producers first. Call without any lock needed by the handler.
 * Closes the queue (pending lines may be discarded) and joins the task.
 * A failed join retains handles
 * and context for retry; do not free the owning modem until stop succeeds. */
h2_pal_result_t h2_modem_urc_stop(h2_modem_urc_worker_t *worker);

#ifdef __cplusplus
}
#endif
#endif
