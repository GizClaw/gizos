#ifndef H2_BK_DMA2D_TEST_DRIVER_H
#define H2_BK_DMA2D_TEST_DRIVER_H
#include <common/bk_include.h>
enum { DMA2D_CFG_ERROR = 1, DMA2D_TRANS_ERROR = 2, DMA2D_TRANS_COMPLETE = 4 };
enum { DMA2D_CFG_ERROR_ISR, DMA2D_TRANS_ERROR_ISR, DMA2D_TRANS_COMPLETE_ISR, DMA2D_ISR_NUM };
enum { DMA2D_M2M, DMA2D_INPUT_RGB565, DMA2D_OUTPUT_RGB565, TWO_BYTES = 2 };
typedef void (*dma2d_isr_t)(void *user);
typedef struct dma2d_memcpy_pfc {
    void *input_addr, *output_addr;
    unsigned mode, input_alpha, input_color_mode, output_color_mode;
    unsigned src_pixel_byte, dst_pixel_byte;
    unsigned src_frame_width, src_frame_height, dst_frame_width, dst_frame_height;
    unsigned dma2d_width, dma2d_height;
} dma2d_memcpy_pfc_t;
typedef struct dma2d_fill {
    void *frameaddr;
    unsigned frame_xsize, frame_ysize, width, height;
    unsigned color_format, pixel_byte;
    uint16_t color;
} dma2d_fill_t;
int bk_dma2d_driver_init(void);
int bk_dma2d_driver_deinit(void);
int bk_dma2d_register_int_callback_isr(int id, dma2d_isr_t callback, void *user);
int bk_dma2d_int_enable(unsigned mask, bool enable);
int bk_dma2d_stop_transfer(void);
int bk_dma2d_start_transfer(void);
void bk_dma2d_soft_reset(void);
bool bk_dma2d_is_transfer_busy(void);
void bk_dma2d_memcpy_or_pixel_convert(dma2d_memcpy_pfc_t *copy);
int dma2d_fill(dma2d_fill_t *fill);
#endif
