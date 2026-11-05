#ifndef H2_BK_PLATFORM_IMAGE_H
#define H2_BK_PLATFORM_IMAGE_H

#include "h2/pal/hal/h2_pal_power.h"
#include "h2/pal/os/h2_pal_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stable native window IDs; the board partition table owns their geometry. */
#define H2_BK_PRIMARY_PARTITION_ID 1u
#define H2_BK_APP_PARTITION_ID 2u

const h2_pal_image_api_t* h2_bk_platform_image_api(void);
const h2_pal_power_api_t* h2_bk_platform_power_api(void);
/* Arm native rollback after SDK startup, before exposing application services.
 * Legacy SDK A/B arms A as the reset target on every B boot until that boot
 * confirms. Fixed-window App boot has already consumed its trial in the early
 * CP hook, so this is a no-op there. */
int h2_bk_platform_boot_startup(void);

#ifdef __cplusplus
}
#endif
#endif
