#ifndef H2_JIELI_APP_IOSTREAMIKCP_H
#define H2_JIELI_APP_IOSTREAMIKCP_H

#include "h2_loader_app_client.h"

#include <stddef.h>

int h2_jieli_app_iostreamikcp_start(
    h2_loader_app_client_t *client,
    const h2_pal_task_api_t *task,
    const h2_pal_mem_api_t *allocator);

int h2_jieli_app_iostreamikcp_log(const char *data, size_t len);

#endif
