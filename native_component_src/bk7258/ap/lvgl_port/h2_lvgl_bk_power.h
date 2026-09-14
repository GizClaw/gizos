#ifndef H2_LVGL_BK_POWER_H
#define H2_LVGL_BK_POWER_H

#ifdef __cplusplus
extern "C" {
#endif

/** Release DMA2D and its completion semaphore after LVGL draw tasks stop.
 * Returns the BK SDK result; repeated calls after successful release succeed.
 */
int h2_bk_dma2d_suspend(void);

#ifdef __cplusplus
}
#endif

#endif
