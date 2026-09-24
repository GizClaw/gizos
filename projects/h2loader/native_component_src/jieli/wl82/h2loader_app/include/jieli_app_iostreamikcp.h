#ifndef H2_JIELI_APP_IOSTREAMIKCP_H
#define H2_JIELI_APP_IOSTREAMIKCP_H

#include "h2_loader_app_client.h"

#include <stddef.h>

/* Start the boot-lifetime App console from the serialized App startup path.
 * The client and allocator must remain valid for the rest of this boot. If the
 * command task cannot start, a retry with the same client and allocator reuses
 * the physical console; it does not recreate or stop its RX worker. */
int h2_jieli_app_iostreamikcp_start(
    h2_loader_app_client_t *client,
    const h2_pal_task_api_t *task,
    const h2_pal_mem_api_t *allocator);

int h2_jieli_app_iostreamikcp_log(const char *data, size_t len);

/* Open the session-admission gate. Until this is called (or a fallback deadline
 * inside the transport expires), the App answers no SESSION_OPEN, so the boot
 * and confirmation console leaves on the raw pre-session path the host captures
 * losslessly instead of being queued into a reliable tunnel and discarded when
 * the App proceeds. The App calls this once its confirmation has been emitted.
 * Idempotent; sticky once opened. */
void h2_jieli_app_iostreamikcp_admit_sessions(void);

#endif
