#ifndef H2_LVGL_BK_DMA2D_H
#define H2_LVGL_BK_DMA2D_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Render an opaque RGB565 rectangle in the target DMA2D component.
 * Strides are bytes. NULL src selects fill; non-NULL src selects copy.
 * Buffers remain caller-owned and exclusive for this synchronous task call,
 * which waits at most 100 ms for DMA completion. Only the single LVGL software
 * draw unit may call this hook. Return 1 when handled, or 0 for CPU rendering.
 * A failed transfer can change dst; the CPU path must redraw the rectangle.
 * Unrecoverable busy hardware enters target assertion/fail-stop while keeping
 * the buffer borrowed; it cannot safely return to CPU rendering.
 */
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
