#ifndef H2_GIZCLAW_SOCIAL_INTERNAL_H
#define H2_GIZCLAW_SOCIAL_INTERNAL_H

#include "h2_gizclaw_social.h"

#include "h2_gizclaw_rpc.h"
#include "pb.h"

h2_pal_result_t h2_gizclaw_social_create_message_internal(
    h2_gizclaw_service_t *service, uint64_t identity, const void *tag,
    h2_gizclaw_rpc_method_t method, const pb_msgdesc_t *fields,
    const void *message, uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

#endif
