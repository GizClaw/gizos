#include "h2_pal.h"
#include <assert.h>
#include <string.h>

int main(void) {
    const h2_pal_camera_vtable_t empty_vtable = {0};
    const h2_pal_camera_api_t empty_api = {0};
    const h2_pal_camera_api_t missing_callbacks = {.vtable = &empty_vtable};
    const h2_pal_camera_api_t *apis[] = {
        NULL, &empty_api, &missing_callbacks, h2_pal_unsupported_camera_api(),
    };
    for (size_t i = 0; i < sizeof(apis) / sizeof(apis[0]); ++i) {
        h2_pal_camera_frame_t frame;
        memset(&frame, 0xff, sizeof(frame));
        assert(h2_pal_camera_start(apis[i]) == H2_PAL_ERR_UNSUPPORTED);
        assert(h2_pal_camera_acquire(apis[i], &frame) == H2_PAL_ERR_UNSUPPORTED);
        assert(frame.data == NULL && frame.size == 0 && frame.width == 0 &&
               frame.height == 0 && frame.stride == 0 && frame.token == 0);
        assert(h2_pal_camera_acquire(apis[i], NULL) == H2_PAL_ERR_INVALID_ARG);
        assert(h2_pal_camera_release(apis[i], NULL) == H2_PAL_ERR_INVALID_ARG);
        assert(h2_pal_camera_release(apis[i], &frame) == H2_PAL_ERR_UNSUPPORTED);
        assert(h2_pal_camera_stop(apis[i]) == H2_PAL_ERR_UNSUPPORTED);
    }
    return 0;
}
