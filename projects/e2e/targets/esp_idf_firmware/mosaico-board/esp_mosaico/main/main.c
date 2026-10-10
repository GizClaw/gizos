#include "h2_esp_board.h"
#include "h2_chip_board.h"
#include "h2_mosaico_usb_console.h"
#include "h2_esp_target_task_policy.h"
#include "h2_pal_e2e.h"
#include "diagnostics.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_heap_caps.h"

static void report(const char *name, int rc) {
    printf("H2_MOSAICO_CASE name=%s status=%s rc=%d\n", name,
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
    fflush(stdout);
}

static void halt(const char *stage, int rc) {
    for (;;) {
        printf("H2_MOSAICO_HALTED stage=%s rc=%d\n", stage, rc);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static int color_bars(h2_runtime_t *runtime) {
    int rc = h2_pal_display_open(runtime->display);
    if (rc != H2_PAL_OK) return rc;
    h2_display_info_t info;
    rc = h2_pal_display_get_info(runtime->display, &info);
    if (rc != H2_PAL_OK) return rc;
    if (info.width != 480 || info.height != 480) return H2_PAL_ERR_INVALID_STATE;
    uint16_t line[480];
    const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
    for (int x = 0; x < 480; ++x) line[x] = colors[x / 120];
    for (int y = 0; y < 480 && rc == H2_PAL_OK; ++y) {
        const h2_display_rect_t rect = {0, y, 480, 1};
        rc = h2_pal_display_draw_bitmap(runtime->display, &rect, line,
                                       sizeof(line), H2_DISPLAY_PIXEL_RGB565);
    }
    if (rc == H2_PAL_OK) rc = h2_pal_display_set_brightness_percent(runtime->display, 100);
    if (rc == H2_PAL_OK) rc = h2_pal_display_present(runtime->display);
    return rc;
}

/* Small diagnostic font: 5x7 cells, scaled to 10x21 pixels. No GUI dependency. */
static const char glyph_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-:/.,%+?=";
static const uint8_t glyphs[][7] = {
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
 {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
 {14,17,17,15,1,1,14},
 {0,0,0,31,0,0,0},{0,4,4,0,4,4,0},{1,2,2,4,8,8,16},
 {0,0,0,0,0,4,4},{0,0,0,0,4,4,8},{17,2,4,8,16,17,0},
 {0,4,4,31,4,4,0},{14,17,1,2,4,0,4},{0,0,31,0,31,0,0}
};
_Static_assert(sizeof(glyphs) / sizeof(glyphs[0]) == sizeof(glyph_chars) - 1,
               "font mapping mismatch");
static uint16_t screen_row[480 * 24];
static char previous_lines[19][40];
static uint16_t previous_colors[19];
static int imu_rc[3];
static h2_pal_imu_reading_t imu_samples[3];
static int battery_rc;
static h2_pal_battery_reading_t battery_sample;
static unsigned touch_events, button_presses[2];
static int touch_kind, touch_x, touch_y, touch_status;
static int button_status[2], button_state[2];
static unsigned heartbeat;
#define INK 0xffff
#define GOOD 0x7fef
#define WARN 0xffc0
#define BAD 0xfaaa
#define BACKGROUND 0x0841

static int dashboard_line(h2_runtime_t *runtime, int row, uint16_t color,
                          const char *format, ...) {
    char text[40];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (strcmp(previous_lines[row], text) == 0 && previous_colors[row] == color)
        return H2_PAL_OK;
    for (size_t i = 0; i < sizeof(screen_row) / sizeof(screen_row[0]); ++i)
        screen_row[i] = BACKGROUND;
    for (size_t c = 0; text[c] && c < 38; ++c) {
        const char *g = strchr(glyph_chars, text[c]);
        if (!g) continue;
        for (int y = 0; y < 21; ++y)
            for (int x = 0; x < 10; ++x)
                if (glyphs[g - glyph_chars][y / 3] & (1u << (4 - x / 2)))
                    screen_row[(y + 1) * 480 + 12 + c * 12 + x] = color;
    }
    const h2_display_rect_t rect = {0, row * 24, 480, 24};
    int rc = h2_pal_display_draw_bitmap(runtime->display, &rect, screen_row,
                                       480 * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
    if (rc == H2_PAL_OK) {
        memcpy(previous_lines[row], text, sizeof(text));
        previous_colors[row] = color;
    }
    return rc;
}

static const char *core_status(const h2_pal_e2e_result_t *result, int id) {
    for (size_t i = 0; i < result->case_count; ++i)
        if ((int)result->cases[i].case_id == id)
            return result->cases[i].result == H2_PAL_OK ? "PASS" : "FAIL";
    return "SKIP";
}

static int dashboard(h2_runtime_t *runtime, const h2_pal_e2e_result_t *result, int core_rc) {
    int first = H2_PAL_OK;
#define LINE(...) do { int r = dashboard_line(runtime, __VA_ARGS__); if (first == 0) first = r; } while (0)
    LINE(0, INK, "GIZOS MOSAICO / BOARD E2E");
    const bool core_ok = core_rc == 0 && result->complete && result->failed == 0 &&
                         result->cleanup_result == 0 && result->passed == 8;
    LINE(1, core_ok ? GOOD : BAD, "CORE %s  PASS:%u FAIL:%u", core_ok ? "PASS" : "FAIL",
         (unsigned)result->passed, (unsigned)result->failed);
    const int ids[] = {1, 2, 3, 4, 5, 6, 10, 11};
    const char *names[] = {"TIME", "TIMER", "TASK", "QUEUE", "MUTEX", "SEMAPHORE", "CONDITION", "CONCURRENCY"};
    for (int i = 0; i < 4; ++i) {
        const char *a = core_status(result, ids[i * 2]);
        const char *b = core_status(result, ids[i * 2 + 1]);
        LINE(2 + i, strcmp(a, "PASS") == 0 && strcmp(b, "PASS") == 0 ? GOOD : WARN,
             "%s:%s %s:%s", names[i * 2], a, names[i * 2 + 1], b);
    }
    LINE(6, INK, "AI:CAM/LOG BOOT:BEEP");
    LINE(7, touch_status == 0 ? INK : BAD, "TOUCH RC:%d N:%u XY:%d,%d K:%d",
         touch_status, touch_events, touch_x, touch_y, touch_kind);
    LINE(8, button_status[0] || button_status[1] ? BAD : INK,
         "AI:%d N:%u BOOT:%d N:%u RC:%d/%d", button_state[0], button_presses[0],
         button_state[1], button_presses[1], button_status[0], button_status[1]);
    if (imu_rc[0] == 0)
        LINE(9, INK, "ACC MG:%" PRId32 ",%" PRId32 ",%" PRId32,
             imu_samples[0].accel_mg.x, imu_samples[0].accel_mg.y, imu_samples[0].accel_mg.z);
    else LINE(9, BAD, "BMI270 READ ERROR RC:%d", imu_rc[0]);
    for (int i = 1; i < 3; ++i) {
        if (imu_rc[i] == 0) {
            const uint32_t flags = imu_samples[i].flags;
            char axes[3][12];
            const int32_t values[] = {imu_samples[i].mag_mgauss.x,
                                     imu_samples[i].mag_mgauss.y,
                                     imu_samples[i].mag_mgauss.z};
            const uint32_t saturated[] = {H2_PAL_IMU_MAG_X_SATURATED,
                                          H2_PAL_IMU_MAG_Y_SATURATED,
                                          H2_PAL_IMU_MAG_Z_SATURATED};
            for (int axis = 0; axis < 3; ++axis) {
                if (flags & saturated[axis]) snprintf(axes[axis], sizeof(axes[axis]), "SAT");
                else snprintf(axes[axis], sizeof(axes[axis]), "%" PRId32, values[axis]);
            }
            LINE(9 + i, flags & (saturated[0] | saturated[1] | saturated[2]) ? WARN : INK,
                 "MAG%d X:%s Y:%s Z:%s", i, axes[0], axes[1], axes[2]);
        } else LINE(9 + i, BAD, "MAG%d READ ERROR RC:%d", i, imu_rc[i]);
    }
    if (battery_rc == 0)
        LINE(12, INK, "BAT:%" PRId32 "MV SOC:%u%% READ ONLY", battery_sample.voltage_mv,
             (unsigned)battery_sample.percent_x100 / 100);
    else LINE(12, BAD, "BATTERY READ ERROR RC:%d", battery_rc);
    LINE(13, WARN, "%s", mosaico_diagnostics.battery);
    LINE(14, INK, "%s", mosaico_diagnostics.audio);
    LINE(15, INK, "%s", mosaico_diagnostics.wifi);
    LINE(16, INK, "%s", mosaico_diagnostics.ble);
    LINE(17, WARN, "%s", mosaico_diagnostics.hotplug[0] ? mosaico_diagnostics.hotplug : mosaico_diagnostics.stage);
    LINE(18, INK, "%s LIVE:%u", mosaico_diagnostics.camera, heartbeat++);
#undef LINE
    if (first == H2_PAL_OK) first = h2_pal_display_present(runtime->display);
    return first;
}

static void sample_sensors(h2_runtime_t *runtime) {
    for (uint32_t id = 201; id <= 203; ++id) {
        h2_pal_imu_reading_t sample = {0};
        const int rc = h2_pal_imu_read(runtime->imu, id, &sample);
        imu_rc[id - 201] = rc;
        imu_samples[id - 201] = sample;
        printf("H2_MOSAICO_IMU id=%" PRIu32 " rc=%d flags=%" PRIu32
               " accel_mg=%" PRId32 ",%" PRId32 ",%" PRId32
               " gyro_mdps=%" PRId32 ",%" PRId32 ",%" PRId32
               " mag_mgauss=%" PRId32 ",%" PRId32 ",%" PRId32 "\n",
               id, rc, sample.flags, sample.accel_mg.x, sample.accel_mg.y,
               sample.accel_mg.z, sample.gyro_mdps.x, sample.gyro_mdps.y,
               sample.gyro_mdps.z, sample.mag_mgauss.x, sample.mag_mgauss.y,
               sample.mag_mgauss.z);
    }
    h2_pal_battery_reading_t battery = {0};
    const int rc = h2_pal_input_read_battery(runtime->input, 301, &battery);
    battery_rc = rc;
    battery_sample = battery;
    printf("H2_MOSAICO_BATTERY rc=%d mv=%" PRId32 " percent_x100=%u\n",
           rc, battery.voltage_mv, (unsigned)battery.percent_x100);
}

typedef struct { h2_runtime_t *runtime; const h2_pal_e2e_result_t *result; int core_rc; } screen_context_t;
static void refresh_tests(void *user) {
    screen_context_t *ctx = user;
    const int rc = dashboard(ctx->runtime, ctx->result, ctx->core_rc);
    if (rc != 0) report("dashboard", rc);
}

void app_main(void) {
    int rc = h2_mosaico_usb_console_init();
    if (rc != 0) halt("usb_console", rc);
    /* USB enumerates again after ROM download. Repeat the header for late monitors. */
    for (unsigned i = 0; i < 15; ++i) {
        printf("H2_MOSAICO_BOOT firmware=board-e2e start_in=%u\n", 15 - i);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    h2_mosaico_revision_t revision;
    rc = h2_mosaico_board_revision(&revision);
    report("board_revision", rc);
    if (rc != 0) halt("board_revision", rc);
    printf("H2_MOSAICO_REVISION raw=0x%04x\n", revision.version);
    rc = h2_esp_target_task_policy_install();
    report("task_policy", rc);
    if (rc != H2_PAL_OK) halt("task_policy", rc);
    h2_runtime_config_t config;
    h2_runtime_t *runtime = NULL;
    rc = h2_esp_board_runtime_config(&config);
    report("board_config", rc);
    if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &runtime);
    report("runtime_init", rc);
    if (rc != H2_PAL_OK) halt("runtime_init", rc);
    const h2_pal_e2e_config_t tests = {.suite_mask = H2_PAL_E2E_SUITE_CORE};
    h2_pal_e2e_result_t result;
    rc = h2_pal_e2e_run(runtime, &tests, &result);
    for (size_t i = 0; i < result.case_count; ++i) {
        printf("H2_MOSAICO_CORE case=%u rc=%d\n", (unsigned)result.cases[i].case_id,
               result.cases[i].result);
    }
    printf("H2_MOSAICO_CORE_SUMMARY rc=%d complete=%d passed=%u failed=%u cleanup=%d\n",
           rc, result.complete, (unsigned)result.passed, (unsigned)result.failed,
           result.cleanup_result);
    const int core_rc = rc;
    /* A failed join can retain live tasks; preserve Runtime and stop further tests. */
    if (result.retained_cleanup != NULL) halt("core_cleanup", result.cleanup_result);
    const int display_rc = color_bars(runtime);
    report("display_transfer", display_rc);
    printf("H2_MOSAICO_MANUAL check=red_green_blue_white_bars touch=drag_screen buttons=AI_BOOT\n");
    const int touch_rc = h2_pal_touch_open(runtime->touch);
    report("touch_open", touch_rc);
    touch_status = touch_rc;
    printf("H2_MOSAICO_SKIP camera_hot_remove nand motor update rollback reason=not_exercised\n");
    sample_sensors(runtime);
    screen_context_t screen = {runtime, &result, core_rc};
    mosaico_run_diagnostics(runtime, display_rc == 0 ? refresh_tests : NULL, &screen);
    unsigned cycle = 0;
    int last_button[2] = {-1, -1};
    bool last_view = false;
    uint32_t last_press_ms[2] = {0, 0};
    for (;;) {
        if (mosaico_camera_e2e_poll(runtime)) memset(previous_lines, 0, sizeof(previous_lines));
        if (cycle++ % 100 == 0) {
            sample_sensors(runtime);
            printf("H2_MOSAICO_ALIVE core_passed=%u core_failed=%u\n",
                   (unsigned)result.passed, (unsigned)result.failed);
        }
        for (unsigned i = 0; i < 2; ++i) {
            h2_pal_single_button_reading_t button = {0};
            rc = h2_pal_button_read_single_button(runtime->button, 101 + i, &button);
            button_status[i] = rc;
            button_state[i] = rc == 0 ? (int)button.state : -1;
            if (rc == 0 && button.state == 1 && last_button[i] != 1) {
                const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                if (now_ms - last_press_ms[i] > 250) {
                    last_press_ms[i] = now_ms;
                    ++button_presses[i];
                    if (i == 0) report("camera_view_toggle", mosaico_camera_view_toggle());
                    else {
                        mosaico_audible_test(runtime);
                        if (mosaico_camera_view_active()) last_view = false;
                    }
                }
            }
            if (rc != H2_PAL_OK || last_button[i] != (int)button.state) {
                printf("H2_MOSAICO_BUTTON id=%u rc=%d pressed=%d\n", 101 + i, rc, button.state);
                last_button[i] = button.state;
            }
        }
        if (touch_rc == H2_PAL_OK) {
            h2_pal_touch_event_t event;
            rc = h2_pal_touch_poll_event(runtime->touch, &event);
            if (rc == H2_PAL_OK) {
                touch_status = 0;
                ++touch_events;
                touch_kind = event.kind;
                touch_x = event.x;
                touch_y = event.y;
                printf("H2_MOSAICO_TOUCH kind=%d x=%" PRId32 " y=%" PRId32 "\n",
                                       event.kind, event.x, event.y);
            } else if (rc != H2_PAL_ERR_WOULD_BLOCK) { touch_status = rc; report("touch_poll", rc); }
        }
        const bool camera_view = mosaico_camera_view_active();
        if (display_rc == H2_PAL_OK && camera_view != last_view) {
            for (size_t n = 0; n < sizeof(screen_row) / sizeof(screen_row[0]); ++n) screen_row[n] = BACKGROUND;
            for (int y = 0; y < 456; y += 24) {
                const h2_display_rect_t rect = {0, y, 480, 24};
                rc = h2_pal_display_draw_bitmap(runtime->display, &rect, screen_row, 480 * 2, H2_DISPLAY_PIXEL_RGB565);
                if (rc != 0) { report("view_clear", rc); break; }
            }
            memset(previous_lines, 0, sizeof(previous_lines));
            last_view = camera_view;
        }
        if (camera_view) {
            rc = mosaico_camera_view_tick(runtime);
            if (rc != 0) report("camera_view", rc);
        }
        if (display_rc == H2_PAL_OK && cycle % 25 == 1) {
            if (mosaico_camera_view_active()) {
                dashboard_line(runtime, 0, INK, "CAMERA LIVE / DVP");
                dashboard_line(runtime, 1, INK, "AI:LOG BOOT:BEEP");
                dashboard_line(runtime, 17, WARN, "KEEP CAMERA CONNECTED");
                dashboard_line(runtime, 18, INK, "%s", mosaico_diagnostics.camera);
                rc = h2_pal_display_present(runtime->display);
            } else rc = dashboard(runtime, &result, core_rc);
            if (rc != H2_PAL_OK) report("dashboard", rc);
        }
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
