#ifndef H2_MOSAICO_CAMERA_H
#define H2_MOSAICO_CAMERA_H
#include "h2/pal/hal/h2_pal_camera.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Default Mosaico provider; obtaining it never probes or starts hardware.
 * start initializes the pinned upstream module manager lazily. Left-slot
 * SC101IOT, 1280x720 UYVY; fixed finite dequeue timeout 1000 ms. Upstream
 * mutex waits and scheduler delay are not bounded by that timeout. Call from
 * serialized worker/task context, never ISR; return loans before teardown. */
const h2_pal_camera_api_t* h2_mosaico_camera(void);
#ifdef __cplusplus
}
#endif
#endif
