#include "h2_mosaico_camera.h"
#include "mosaico_module_camera.h"
#include <string.h>
#include <stdio.h>
static mosaico_camera_handle_t camera;
static mosaico_camera_frame_t borrowed;
static uint64_t token;
static bool streaming, loaned;
static int result(esp_err_t rc) {
    switch (rc) {
    case ESP_OK: return H2_PAL_OK;
    case ESP_ERR_TIMEOUT: return H2_PAL_ERR_TIMEOUT;
    case ESP_ERR_NO_MEM: return H2_PAL_ERR_NO_MEMORY;
    case ESP_ERR_INVALID_ARG: return H2_PAL_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE: return H2_PAL_ERR_INVALID_STATE;
    case ESP_ERR_NOT_FOUND: return H2_PAL_ERR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return H2_PAL_ERR_UNSUPPORTED;
    default: return H2_PAL_ERR_IO;
    }
}
static int stop(void *user) {
    (void)user;
    if (loaned) return H2_PAL_ERR_BUSY;
    if (borrowed.data) {
        const int returned = result(mosaico_camera_return_frame(camera, &borrowed));
        if (returned != H2_PAL_OK) return returned;
        memset(&borrowed, 0, sizeof(borrowed));
    }
    if (!camera) return H2_PAL_OK;
    streaming = false; /* Failed teardown is retryable, never usable for capture. */
    const int rc = result(mosaico_camera_del(camera));
    if (rc == H2_PAL_OK) { camera = NULL; streaming = false; }
    return rc;
}
static int start(void *user) {
    (void)user;
    if (camera) return H2_PAL_ERR_BUSY;
    mosaico_camera_config_t cfg = MOSAICO_CAMERA_DEFAULT_CONFIG();
    cfg.slot = MOSAICO_MODULE_MGR_SLOT_LEFT;
    cfg.width = 1280;
    cfg.height = 720;
    cfg.apply_module_tuning = false;
    int rc = result(mosaico_camera_new(&cfg, &camera));
    if (rc == H2_PAL_OK) rc = result(mosaico_camera_open(camera));
    if (rc == H2_PAL_OK) rc = result(mosaico_camera_start_stream(camera));
    if (rc == H2_PAL_OK) streaming = true;
    else if (camera) {
        const int cleanup = stop(NULL);
        if (cleanup != H2_PAL_OK) return cleanup;
    }
    return rc;
}
static int acquire(void *user, h2_pal_camera_frame_t *out) {
    (void)user;
    if (!out) return H2_PAL_ERR_INVALID_ARG;
    if (!streaming || !camera) return H2_PAL_ERR_INVALID_STATE;
    if (borrowed.data) return H2_PAL_ERR_BUSY;
    int rc = result(mosaico_camera_get_frame(camera, &borrowed));
    if (rc != H2_PAL_OK) return rc;
    /* esp_video may report zero bytesperline for packed UYVY. The official
     * Mosaico preview uses width * 2 in that case. Normalize before PAL export. */
    if (!borrowed.bytes_per_line && borrowed.width <= UINT32_MAX / 2)
        borrowed.bytes_per_line = borrowed.width * 2;
    if (!borrowed.data || borrowed.pixel_format != V4L2_PIX_FMT_UYVY || !borrowed.width ||
        (borrowed.width & 1) || !borrowed.height || borrowed.width > UINT32_MAX / 2 ||
        borrowed.bytes_per_line < borrowed.width * 2 ||
        borrowed.height > SIZE_MAX / borrowed.bytes_per_line ||
        borrowed.size < (size_t)borrowed.bytes_per_line * borrowed.height) {
        printf("H2_MOSAICO_CAMERA_FORMAT width=%lu height=%lu stride=%lu size=%zu format=0x%08lx\n",
               (unsigned long)borrowed.width, (unsigned long)borrowed.height,
               (unsigned long)borrowed.bytes_per_line, borrowed.size, (unsigned long)borrowed.pixel_format);
        rc = result(mosaico_camera_return_frame(camera, &borrowed));
        if (rc == H2_PAL_OK) memset(&borrowed, 0, sizeof(borrowed));
        return rc == H2_PAL_OK ? H2_PAL_ERR_FORMAT : rc;
    }
    loaned = true;
    ++token;
    if (!token) ++token;
    *out = (h2_pal_camera_frame_t){.data = borrowed.data, .size = borrowed.size,
        .width = borrowed.width, .height = borrowed.height,
        .stride = borrowed.bytes_per_line, .token = token};
    return H2_PAL_OK;
}
static int release(void *user, const h2_pal_camera_frame_t *frame) {
    (void)user;
    if (!frame) return H2_PAL_ERR_INVALID_ARG;
    if (!camera || !loaned || !borrowed.data || frame->token != token || frame->data != borrowed.data)
        return H2_PAL_ERR_INVALID_STATE;
    const int rc = result(mosaico_camera_return_frame(camera, &borrowed));
    if (rc == H2_PAL_OK) { memset(&borrowed, 0, sizeof(borrowed)); loaned = false; }
    return rc;
}
const h2_pal_camera_api_t *h2_mosaico_camera(void) {
    static const h2_pal_camera_vtable_t ops = {.start=start, .acquire=acquire, .release=release, .stop=stop};
    static const h2_pal_camera_api_t api = {.vtable=&ops};
    return &api;
}
