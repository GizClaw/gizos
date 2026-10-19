#ifndef H2_LVGL_BK_POWER_H
#define H2_LVGL_BK_POWER_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Release target DMA2D after all LVGL draw workers stop.
 * Task context only; callers serialize with rendering and lifecycle changes.
 * Returns the SDK result. Successful release is idempotent and allows a new
 * LVGL instance to initialize acceleration. Disabled-driver builds succeed.
 */
int h2_bk_dma2d_suspend(void);

#ifdef __cplusplus
}
#endif
#endif
