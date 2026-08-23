#ifndef H2_ESP_H2LOADER_IOSTREAMIKCP_H
#define H2_ESP_H2LOADER_IOSTREAMIKCP_H

#include "h2_loader_app_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Starts an app-side synchronous H2Loader command server task.
 *
 * The task accepts one reliable IO Stream iKCP session at a time. A replacement
 * request resets the command parser before serving the new session. All
 * transport state is component-owned for the lifetime of the task. Call this
 * once during app startup; a repeated successful start returns
 * H2_PAL_ERR_INVALID_STATE.
 *
 * @param client Borrowed initialized app client that must outlive the task.
 * @param task Borrowed task PAL used to create the server task.
 * @param allocator Borrowed allocator retained by the transport.
 * @param stack_size Server task minimum stack size in bytes.
 * @return H2_PAL_OK when the task starts, otherwise a startup error.
 */
int h2_esp_h2loader_app_iostreamikcp_start(
    h2_loader_app_client_t *client,
    const h2_pal_task_api_t *task,
    const h2_pal_mem_api_t *allocator,
    size_t stack_size);

#ifdef __cplusplus
}
#endif

#endif
