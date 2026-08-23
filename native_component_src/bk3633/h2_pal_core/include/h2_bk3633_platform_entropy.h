#ifndef H2_BK3633_PLATFORM_ENTROPY_H
#define H2_BK3633_PLATFORM_ENTROPY_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fill caller-owned storage from the BK3633 hardware TRNG.
 *
 * The function performs a bounded poll for every 32-bit word and applies a
 * continuous repetition check across calls. On failure it clears the complete
 * caller buffer before returning.
 *
 * @param user Reserved callback context; currently ignored.
 * @param out Caller-owned output storage, or NULL only when @p len is zero.
 * @param len Number of bytes to fill.
 * @return H2_PAL_OK after filling all bytes, or a stable PAL error.
 */
int h2_bk3633_platform_entropy_fill(void *user, uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif
