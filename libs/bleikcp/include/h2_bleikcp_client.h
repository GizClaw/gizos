#ifndef H2_BLEIKCP_CLIENT_H
#define H2_BLEIKCP_CLIENT_H

#include "h2_bleikcp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_bleikcp_client_open(
    const h2_bleikcp_api_t *api,
    const h2_bleikcp_config_t *config,
    uint16_t conn_handle,
    uint16_t negotiated_att_mtu,
    h2_bleikcp_t **out_stream);

#ifdef __cplusplus
}
#endif

#endif
