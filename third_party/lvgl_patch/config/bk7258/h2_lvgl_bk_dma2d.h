#ifndef H2_LVGL_BK_DMA2D_H
#define H2_LVGL_BK_DMA2D_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called synchronously from the single LVGL software draw unit. Returning zero
 * leaves unsupported or failed operations to LVGL's original CPU renderer. */
int h2_bk_dma2d_rgb565(void *dst, const void *src, int32_t width,
                     int32_t height, uint32_t dst_stride,
                     uint32_t src_stride, uint16_t color);

#ifdef __cplusplus
}
#endif

#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565(dsc) \
  (h2_bk_dma2d_rgb565((dsc)->dest_buf, NULL, (dsc)->dest_w, \
     (dsc)->dest_h, (dsc)->dest_stride, 0, lv_color_to_u16((dsc)->color)) \
       ? LV_RESULT_OK : LV_RESULT_INVALID)
#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565(dsc) \
  (h2_bk_dma2d_rgb565((dsc)->dest_buf, (dsc)->src_buf, (dsc)->dest_w, \
     (dsc)->dest_h, (dsc)->dest_stride, (dsc)->src_stride, 0) \
       ? LV_RESULT_OK : LV_RESULT_INVALID)

#endif
