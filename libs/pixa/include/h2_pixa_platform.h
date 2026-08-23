#ifndef H2_PIXA_PLATFORM_H
#define H2_PIXA_PLATFORM_H

#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "pixa_osal.h"
#include "pixa_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Borrowed PAL capabilities used by one PIXA platform bridge.
 */
typedef struct h2_pixa_platform_config {
    /** Filesystem API borrowed for the complete initialized lifetime. */
    const h2_pal_fs_api_t *fs;
    /** Memory API borrowed for the complete initialized lifetime. */
    const h2_pal_mem_api_t *mem;
} h2_pixa_platform_config_t;

/**
 * @brief Caller-owned bridge from Firmwares PAL APIs to upstream PIXA APIs.
 *
 * Initialize this state before requesting its OSAL or allocator objects. The
 * borrowed PAL APIs must remain valid until deinitialization. An initialized
 * state must remain at the same address and must not be copied.
 */
typedef struct h2_pixa_platform {
    const h2_pal_fs_api_t *fs;
    const h2_pal_mem_api_t *mem;
    pixa_osal_api_t osal;
    pixa_alloc_t allocator;
    int initialized;
} h2_pixa_platform_t;

/**
 * @brief Initialize or replace a caller-owned PIXA platform bridge in place.
 *
 * @param platform Caller-owned state to initialize.
 * @param config Borrowed filesystem and memory APIs.
 * @return H2_PAL_OK on success or H2_PAL_ERR_INVALID_ARG for an incomplete
 *         configuration.
 */
h2_pal_result_t h2_pixa_platform_init(
    h2_pixa_platform_t *platform,
    const h2_pixa_platform_config_t *config);

/**
 * @brief Invalidate a PIXA platform bridge without destroying borrowed PAL APIs.
 *
 * @param platform Caller-owned state; NULL is accepted.
 */
void h2_pixa_platform_deinit(h2_pixa_platform_t *platform);

/**
 * @brief Borrow the upstream PIXA filesystem API.
 *
 * @return The initialized OSAL API, or NULL after deinitialization.
 */
const pixa_osal_api_t *h2_pixa_platform_osal(
    const h2_pixa_platform_t *platform);

/**
 * @brief Borrow the upstream PIXA allocator.
 *
 * @return The initialized allocator, or NULL after deinitialization.
 */
const pixa_alloc_t *h2_pixa_platform_allocator(
    const h2_pixa_platform_t *platform);

#ifdef __cplusplus
}
#endif

#endif
