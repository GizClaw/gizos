#ifndef H2_DISPLAY_VIEWPORT_H
#define H2_DISPLAY_VIEWPORT_H

#include "h2/pal/hal/h2_pal_display.h"
#include "h2/pal/os/h2_pal_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_display_viewport h2_display_viewport_t;

/** @brief Create a translated, unscaled window over a real PAL Display.
 * mem and backend are borrowed until destroy; backend must be closed and
 * exclusively owned by this adapter's caller. area uses backend coordinates
 * and must fit the actual display when open is called. No framebuffer is
 * allocated. Operations are serialized by the caller, in task context.
 * Allocation failure returns NO_MEMORY; invalid arguments return INVALID_ARG.
 * On failure *out_viewport is NULL. Creation does not open the display.
 */
h2_pal_result_t h2_display_viewport_create(
    const h2_pal_mem_api_t *mem, const h2_pal_display_api_t *backend,
    const h2_display_rect_t *area, h2_display_viewport_t **out_viewport);

/** @brief Borrow the adapter API until destroy (NULL instance returns NULL).
 * Open/close are idempotent and own the backend's open/close lifecycle.
 * Info exposes the window size and actual native format. Draw requires a
 * rectangle fully inside the window, checks format/stride/span arithmetic,
 * and translates its origin; pixels are borrowed only during the call.
 * Present and brightness affect the real backend. Backend errors propagate.
 * Failed open after acquiring the backend retains it for close/destroy retry.
 */
const h2_pal_display_api_t *h2_display_viewport_api(h2_display_viewport_t *viewport);

/** @brief Close the backend and free adapter state. NULL succeeds.
 * Stop all users first. On close failure retain the instance for retry; on
 * success all borrowed adapter API pointers become invalid.
 */
h2_pal_result_t h2_display_viewport_destroy(h2_display_viewport_t *viewport);

#ifdef __cplusplus
}
#endif
#endif
