#ifndef H2_MOSAICO_CAMERA_H
#define H2_MOSAICO_CAMERA_H
#include "h2/pal/hal/h2_pal_camera.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Optional native component, initialized lazily; only the left slot is used.
 * Initialize the official module manager before first use. Calls must be
 * serialized by the caller. Fixed 1280x720 UYVY; acquire timeout is 1000 ms. */
const h2_pal_camera_api_t *h2_mosaico_camera(void);
#ifdef __cplusplus
}
#endif
#endif
