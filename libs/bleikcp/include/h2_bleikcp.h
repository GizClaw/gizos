#ifndef H2_BLEIKCP_H
#define H2_BLEIKCP_H

#include "h2_bleikcp_client.h"
#include "h2_bleikcp_server.h"
#include "h2_bleikcp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_bleikcp_close(h2_bleikcp_t *stream);
int h2_bleikcp_read(
    h2_bleikcp_t *stream,
    uint8_t *out,
    size_t out_size,
    size_t *out_len,
    uint32_t timeout_ms);
int h2_bleikcp_write(
    h2_bleikcp_t *stream,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms);
/**
 * @brief Wait for locally queued and KCP-pending output to be acknowledged.
 *
 * A published fatal status takes precedence over completion. Otherwise, a
 * normal disconnect does not replace a completed result: this function
 * returns H2_PAL_OK when both the local TX queue and the published KCP
 * waitsnd count are empty. It returns H2_PAL_ERR_CLOSED when output remains
 * pending after a normal disconnect.
 *
 * @param stream Stream whose published output state is inspected.
 * @param timeout_ms Maximum wait in milliseconds, zero for non-blocking, or
 * H2_PAL_SYNC_WAIT_FOREVER.
 * @return H2_PAL_OK, the published fatal status, H2_PAL_ERR_INVALID_ARG,
 * H2_PAL_ERR_CLOSED, H2_PAL_ERR_WOULD_BLOCK, H2_PAL_ERR_TIMEOUT, or an
 * underlying wait error.
 */
int h2_bleikcp_flush(h2_bleikcp_t *stream, uint32_t timeout_ms);
int h2_bleikcp_get_stats(
    h2_bleikcp_t *stream,
    h2_bleikcp_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif
