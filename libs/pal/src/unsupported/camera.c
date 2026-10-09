#include "h2_pal.h"

static h2_pal_result_t unsupported_camera_control(void *user) {
    (void)user;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_camera_acquire(void *user, h2_pal_camera_frame_t *frame) {
    (void)user;
    if (frame != NULL) memset(frame, 0, sizeof(*frame));
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_camera_release(void *user, const h2_pal_camera_frame_t *frame) {
    (void)user;
    (void)frame;
    return H2_PAL_ERR_UNSUPPORTED;
}

const h2_pal_camera_api_t *h2_pal_unsupported_camera_api(void) {
    static const h2_pal_camera_vtable_t vtable = {
        .start = unsupported_camera_control,
        .acquire = unsupported_camera_acquire,
        .release = unsupported_camera_release,
        .stop = unsupported_camera_control,
    };
    static const h2_pal_camera_api_t api = {.vtable = &vtable};
    return &api;
}
