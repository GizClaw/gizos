#ifndef H2_SCTP_H
#define H2_SCTP_H

#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/net/h2_pal_sctp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque owner of independent H2SCTP associations. */
typedef struct h2_sctp h2_sctp_t;

/** Borrowed dependencies used by one H2SCTP provider. */
typedef struct h2_sctp_config {
    /** Allocator that remains valid until h2_sctp_destroy() succeeds. */
    const h2_pal_mem_api_t *mem;
    /** Cryptographic random source that remains valid until destroy. */
    const h2_pal_crypto_api_t *crypto;
    /**
     * Optional allocator for each association's outbound packet pool; NULL
     * uses mem. Every outbound packet is built directly in a pooled buffer
     * and handed to emit_packet from there, so an integration whose
     * transport wants DMA-capable memory can place the pool accordingly.
     * Must remain valid until h2_sctp_destroy() succeeds.
     */
    const h2_pal_mem_api_t *packet_mem;
    /**
     * Pooled packet buffers per association, each max_packet_size bytes.
     * 0 selects the default (H2_SCTP_DEFAULT_PACKET_POOL_SIZE): one buffer
     * being built plus one retained while emit_packet reports WOULD_BLOCK.
     * Larger values are clamped to H2_SCTP_MAX_PACKET_POOL_SIZE. With the
     * pool exhausted a DATA send stays queued and reports WOULD_BLOCK until
     * a later service pass, and a control chunk is retried like an
     * allocation failure.
     */
    size_t packet_pool_size;
} h2_sctp_config_t;

#define H2_SCTP_DEFAULT_PACKET_POOL_SIZE 2u
#define H2_SCTP_MAX_PACKET_POOL_SIZE 32u

/**
 * Creates an independent provider with no process-global mutable state.
 *
 * The config and API objects are borrowed. The provider copies their pointer
 * values and never destroys either backend. On every failure, out_provider is
 * cleared and all partially allocated state is released.
 */
h2_pal_result_t h2_sctp_create(
    const h2_sctp_config_t *config,
    h2_sctp_t **out_provider);

/**
 * Returns the borrowed PAL SCTP view owned by provider.
 *
 * The view remains valid until h2_sctp_destroy() succeeds. NULL provider
 * returns NULL.
 */
const h2_pal_sctp_api_t *h2_sctp_api(h2_sctp_t *provider);

/**
 * Destroys a provider after every association has been closed.
 *
 * A NULL provider is a successful no-op. Live associations cause
 * H2_PAL_ERR_INVALID_STATE without clearing or damaging the provider.
 */
h2_pal_result_t h2_sctp_destroy(h2_sctp_t **provider);

#ifdef __cplusplus
}
#endif

#endif
