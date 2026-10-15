#include "diagnostics.h"
#include "h2_mosaico_camera.h"
#include "mosaico_module_mgr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static bool manager_ready, tested, halted;
static int last_presence[2] = {-1, -1};
static bool seen_absent[2];
static unsigned inserted[2], removed[2];
static uint32_t last_poll;
static unsigned passed, failed;
static bool view_active;
static uint16_t *view_pixels;
static h2_pal_camera_frame_t view_frame;
static uint32_t view_last_frame;
static unsigned view_frames;
static void check(const char *name, int actual, int expected) {
    bool ok = actual == expected;
    passed += ok; failed += !ok;
    printf("H2_MOSAICO_CAMERA_CASE name=%s rc=%d expected=%d pass=%d\n", name, actual, expected, ok);
}
static int clamp(int x) { return x < 0 ? 0 : x > 255 ? 255 : x; }
static int preview(h2_runtime_t *runtime, const h2_pal_camera_frame_t *f, uint16_t *pixels, h2_display_rect_t rect) {
    /* Nearest-neighbor UYVY preview. Sensor orientation is intentionally kept
     * unchanged so visual orientation remains a separate manual check. */
    for (unsigned y = 0; y < (unsigned)rect.height; ++y) {
        const uint8_t *row = f->data + (size_t)(y * f->height / (unsigned)rect.height) * f->stride;
        for (unsigned x = 0; x < (unsigned)rect.width; ++x) {
            unsigned sx = x * f->width / (unsigned)rect.width;
            const uint8_t *p = row + (sx & ~1u) * 2;
            int yy = p[(sx & 1) ? 3 : 1] - 16, u = p[0] - 128, v = p[2] - 128;
            int r = clamp((298 * yy + 409 * v + 128) / 256);
            int g = clamp((298 * yy - 100 * u - 208 * v + 128) / 256);
            int b = clamp((298 * yy + 516 * u + 128) / 256);
            pixels[y * (unsigned)rect.width + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    int rc = h2_pal_display_draw_bitmap(runtime->display, &rect, pixels, (size_t)rect.width * 2, H2_DISPLAY_PIXEL_RGB565);
    return rc == 0 ? h2_pal_display_present(runtime->display) : rc;
}
static void capture_test(h2_runtime_t *runtime) {
    const h2_pal_camera_api_t *api = h2_mosaico_camera();
    uint16_t *pixels = heap_caps_malloc(240 * 192 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    passed = failed = 0;
    unsigned frames = 0;
    if (!pixels) { check("preview-memory", H2_PAL_ERR_NO_MEMORY, 0); goto finish; }
    h2_pal_camera_frame_t frame;
    check("acquire-before-start", h2_pal_camera_acquire(api, &frame), H2_PAL_ERR_INVALID_STATE);
    check("null-frame", h2_pal_camera_acquire(api, NULL), H2_PAL_ERR_INVALID_ARG);
    for (unsigned round = 0; round < 2; ++round) {
        int rc = h2_pal_camera_start(api);
        check(round ? "restart" : "start", rc, 0);
        if (rc != 0) {
            int cleanup = h2_pal_camera_stop(api);
            check("start-failure-cleanup", cleanup, 0);
            halted = cleanup != 0;
            break;
        }
        check("double-start", h2_pal_camera_start(api), H2_PAL_ERR_BUSY);
        for (unsigned i = 0; i < 20; ++i) {
            rc = h2_pal_camera_acquire(api, &frame);
            if (rc != 0) { check("acquire", rc, 0); break; }
            if (!i) {
                printf("H2_MOSAICO_CAMERA_FRAME round=%u width=%lu height=%lu stride=%lu bytes=%zu\n",
                       round, (unsigned long)frame.width, (unsigned long)frame.height,
                       (unsigned long)frame.stride, frame.size);
                h2_pal_camera_frame_t other;
                check("borrow-while-held", h2_pal_camera_acquire(api, &other), H2_PAL_ERR_BUSY);
                check("stop-while-held", h2_pal_camera_stop(api), H2_PAL_ERR_BUSY);
            }
            rc = preview(runtime, &frame, pixels, (h2_display_rect_t){120, 144, 240, 192});
            const int returned = h2_pal_camera_release(api, &frame);
            if (rc != 0) check("preview", rc, 0);
            if (returned != 0) { check("return-frame", returned, 0); view_frame = frame; halted = true; break; }
            if (!i) check("double-return", h2_pal_camera_release(api, &frame), H2_PAL_ERR_INVALID_STATE);
            if (rc != 0) break;
            ++frames;
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        rc = h2_pal_camera_stop(api);
        check("stop-cleanup", rc, 0);
        if (rc != 0 || halted) { halted = true; break; }
        check("acquire-after-stop", h2_pal_camera_acquire(api, &frame), H2_PAL_ERR_INVALID_STATE);
    }
    check("40-preview-frames", frames == 40 ? 0 : H2_PAL_ERR_IO, 0);
finish:
    heap_caps_free(pixels);
    snprintf(mosaico_diagnostics.camera, sizeof(mosaico_diagnostics.camera),
             "CAM:%s FR:%u P:%u F:%u", halted ? "HALTED" : failed ? "FAIL" : "PASS", frames, passed, failed);
    printf("H2_MOSAICO_CAMERA_E2E %s visual_confirmation=REQUIRED\n", mosaico_diagnostics.camera);
}
void mosaico_camera_e2e_init(void) {
    const int rc = mosaico_module_mgr_init(NULL);
    manager_ready = rc == ESP_OK;
    printf("H2_MOSAICO_MODULE_MANAGER init_rc=%d camera_hot_remove=UNSUPPORTED\n", rc);
    snprintf(mosaico_diagnostics.hotplug, sizeof(mosaico_diagnostics.hotplug),
             manager_ready ? "HOT INSERT: WAITING" : "HOT INSERT: INIT ERROR");
}
/* Called by the main diagnostic loop. Manager owns all EEPROM scans, debounce,
 * descriptor CRC validation and GPIO leases; never scan GPIO14 independently
 * while DVP owns it. Boot presence is not counted as a hot insertion. */
bool mosaico_camera_e2e_poll(h2_runtime_t *runtime) {
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (view_active || !manager_ready || now - last_poll < 500) return false;
    last_poll = now;
    bool run = false;
    for (unsigned slot = 0; slot < 2; ++slot) {
        mosaico_module_mgr_info_t info;
        if (mosaico_module_mgr_get_info(slot, &info) != ESP_OK) continue;
        if ((int)info.presence != last_presence[slot]) {
            if (info.presence == MOSAICO_MODULE_PRESENCE_ABSENT) {
                seen_absent[slot] = true;
                if (last_presence[slot] == MOSAICO_MODULE_PRESENCE_PRESENT) ++removed[slot];
                if (!slot) tested = false;
            } else if (info.presence == MOSAICO_MODULE_PRESENCE_PRESENT && seen_absent[slot]) {
                ++inserted[slot];
                seen_absent[slot] = false;
            }
            printf("H2_MOSAICO_HOTPLUG slot=%u presence=%d descriptor=%d type=0x%02x insert=%u remove=%u error=%d\n",
                   slot, info.presence, info.descriptor_state, info.eeprom.board_type,
                   inserted[slot], removed[slot], info.last_error);
            last_presence[slot] = info.presence;
        }
        if (!slot && info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
            info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_INVALID) {
            snprintf(mosaico_diagnostics.camera, sizeof(mosaico_diagnostics.camera), "CAM:INVALID MODULE DESCRIPTOR");
        }
        if (!slot && !tested && !halted && info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
            info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID &&
            info.eeprom.board_type == MOSAICO_BOARD_TYPE_CAMERA) {
            tested = true; run = true;
        }
    }
    snprintf(mosaico_diagnostics.hotplug, sizeof(mosaico_diagnostics.hotplug),
             "HOT L:%u/%u R:%u/%u %s", inserted[0], removed[0], inserted[1], removed[1],
             inserted[0] + inserted[1] ? "SEEN" : "WAIT");
    if (run) capture_test(runtime);
    return run;
}

bool mosaico_camera_view_active(void) { return view_active; }
static int view_stop(void) {
    const h2_pal_camera_api_t *api = h2_mosaico_camera();
    int rc = 0;
    if (view_frame.data) {
        rc = h2_pal_camera_release(api, &view_frame);
        if (rc != 0) return rc;
        memset(&view_frame, 0, sizeof(view_frame));
    }
    rc = h2_pal_camera_stop(api);
    if (rc != 0) return rc;
    heap_caps_free(view_pixels);
    view_pixels = NULL;
    view_active = false;
    printf("H2_MOSAICO_CAMERA_VIEW mode=LOG frames=%u cleanup=0\n", view_frames);
    return 0;
}
int mosaico_camera_view_toggle(void) {
    if (view_active || halted) {
        const int rc = view_stop();
        if (rc == 0) halted = false;
        return rc;
    }
    if (!manager_ready || halted) return H2_PAL_ERR_INVALID_STATE;
    view_pixels = heap_caps_malloc(480 * 272 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!view_pixels) return H2_PAL_ERR_NO_MEMORY;
    int rc = h2_pal_camera_start(h2_mosaico_camera());
    if (rc != 0) {
        const int cleanup = view_stop();
        halted = cleanup != 0;
        snprintf(mosaico_diagnostics.camera, sizeof(mosaico_diagnostics.camera), "CAM OPEN RC:%d CLEANUP:%d", rc, cleanup);
        return rc;
    }
    view_active = true;
    view_frames = 0;
    view_last_frame = 0;
    printf("H2_MOSAICO_CAMERA_VIEW mode=CAMERA start=0\n");
    return 0;
}
int mosaico_camera_view_tick(h2_runtime_t *runtime) {
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (!view_active || now - view_last_frame < 150) return 0;
    view_last_frame = now;
    const h2_pal_camera_api_t *api = h2_mosaico_camera();
    /* A failed return is retried before acquiring any new frame. */
    if (view_frame.data) {
        int rc = h2_pal_camera_release(api, &view_frame);
        if (rc != 0) return rc;
        memset(&view_frame, 0, sizeof(view_frame));
    }
    int rc = h2_pal_camera_acquire(api, &view_frame);
    if (rc == 0) {
        rc = preview(runtime, &view_frame, view_pixels, (h2_display_rect_t){0, 96, 480, 272});
        const int returned = h2_pal_camera_release(api, &view_frame);
        if (returned != 0) return returned;
        memset(&view_frame, 0, sizeof(view_frame));
    }
    if (rc != 0) {
        const int cleanup = view_stop();
        snprintf(mosaico_diagnostics.camera, sizeof(mosaico_diagnostics.camera), "CAM VIEW RC:%d CLEANUP:%d", rc, cleanup);
        return rc;
    }
    ++view_frames;
    if (view_frames % 30 == 0) printf("H2_MOSAICO_CAMERA_VIEW frames=%u rc=0\n", view_frames);
    snprintf(mosaico_diagnostics.camera, sizeof(mosaico_diagnostics.camera), "CAM LIVE FR:%u", view_frames);
    return 0;
}
