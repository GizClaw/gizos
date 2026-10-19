#ifndef H2_PAL_DISPLAY_E2E_H
#define H2_PAL_DISPLAY_E2E_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    H2_PAL_DISPLAY_E2E_CASE_COUNT = 0
#define H2_PAL_DISPLAY_CASE(id, fn) +1
#include "h2_pal_display_cases.inc"
#undef H2_PAL_DISPLAY_CASE
};
typedef struct h2_pal_display_e2e_case {
    const char *id;
    int result;
    int ran;
} h2_pal_display_e2e_case_t;
typedef struct h2_pal_display_e2e_result {
    h2_pal_display_e2e_case_t cases[H2_PAL_DISPLAY_E2E_CASE_COUNT];
    size_t passed, failed, not_run, observations;
    int complete, qualified, cleanup;
} h2_pal_display_e2e_result_t;
/* Expected pixels are an App-owned independent reference, RGB565 host-endian.
 * Observer must read actual renderer/canvas/view output or identify its weaker
 * panel-transfer evidence explicitly. Borrow all pointers only during call. */
typedef int (*h2_pal_display_e2e_observe_fn)(void *user, const uint16_t *pixels,
                                             int width, int height,
                                             uint32_t brightness,
                                             const char *case_id);
typedef struct h2_pal_display_e2e_config {
    /* Optional borrowed Memory API for large bitmap working sets. NULL uses
     * Runtime Mem; owner must outlive the run. Core ownership is unaffected. */
    const h2_pal_mem_api_t *working_mem;
    uint32_t supported_formats; /* bit 1 << h2_display_pixel_format_t */
    int clips_rectangles;
    h2_pal_display_e2e_observe_fn observe;
    void *user;
} h2_pal_display_e2e_config_t;
/* Borrow Runtime and Display owner. Stops at first failing case, attempts
 * close, frees all App buffers, and never confirms a managed image. */
int h2_pal_display_e2e_run(h2_runtime_t *runtime,
                           const h2_pal_display_e2e_config_t *config,
                           h2_pal_display_e2e_result_t *result);
/* Shared machine-readable ledger; output is bounded by the static registry. */
void h2_pal_display_e2e_print(const h2_pal_display_e2e_result_t *result,
                              const char *platform, int rc, int teardown);
/* Compare rendered RGBA/BGRA 8-bit pixels against the reference with tolerance
 * for platform channel quantization. This consumes actual capture pixels. */
int h2_pal_display_e2e_compare(const uint8_t *actual, size_t stride, int bgr,
                               const uint16_t *expected, int width, int height,
                               uint32_t brightness, unsigned tolerance);
#ifdef __cplusplus
}
#endif
#endif
