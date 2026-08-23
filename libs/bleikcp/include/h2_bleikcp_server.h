#ifndef H2_BLEIKCP_SERVER_H
#define H2_BLEIKCP_SERVER_H

#include "h2_bleikcp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* stream is borrowed for the callback duration and must not be closed or retained. */
typedef int (*h2_bleikcp_server_handler_fn)(
    void *user,
    h2_bleikcp_t *stream,
    uint16_t conn_handle);

int h2_bleikcp_server_open(
    const h2_bleikcp_api_t *api,
    const h2_bleikcp_config_t *config,
    h2_bleikcp_server_handler_fn handler,
    void *handler_user,
    h2_bleikcp_server_t **out_server);
int h2_bleikcp_server_close(h2_bleikcp_server_t *server);

#ifdef __cplusplus
}
#endif

#endif
