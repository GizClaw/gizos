#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <os/os.h>
#include <driver/dma2d.h>
#include <armstar.h>
#include "h2_lvgl_bk_dma2d.h"
#include "h2_lvgl_bk_power.h"

static void sync_cache(void *address, long bytes) {
  if (SCB->CLIDR & SCB_CLIDR_DC_Msk) {
    SCB_CleanInvalidateDCache_by_Addr(address, bytes);
  }
}

/* LVGL is configured with one draw unit. This component exclusively owns
 * DMA2D; SPI display transfers use the separate general-purpose DMA engine. */
static beken_semaphore_t completion;
static volatile int transfer_error;
static int disabled;
static unsigned completed;
static unsigned verified_operations;

static void transfer_done(void *arg) {
  (void)arg;
  rtos_set_semaphore(&completion);
}

static void transfer_failed(void *arg) {
  (void)arg;
  transfer_error = 1;
  rtos_set_semaphore(&completion);
}

int h2_bk_dma2d_rgb565(void *dst, const void *src, int32_t width,
                     int32_t height, uint32_t dst_stride,
                     uint32_t src_stride, uint16_t color) {
  static unsigned calls;
  if (++calls == 1) {
    printf("H2_DMA2D entry width=%ld height=%ld stride=%lu\n",
           (long)width, (long)height, (unsigned long)dst_stride);
  }
  if (disabled || dst == NULL || width <= 0 || height <= 0 ||
      width > 16383 || height > 65535 || (uint64_t)width * height < 128 ||
      dst_stride % 2 || dst_stride / 2 < (uint32_t)width ||
      dst_stride / 2 > 65535 ||
      dst_stride / 2 - (uint32_t)width > 16383 || ((uintptr_t)dst & 1) ||
      (src && (src_stride % 2 || src_stride / 2 < (uint32_t)width ||
               src_stride / 2 > 65535 ||
               src_stride / 2 - (uint32_t)width > 16383 || ((uintptr_t)src & 1)))) {
    return 0;
  }
  const uint64_t dst_span = (uint64_t)(height - 1) * dst_stride + width * 2u;
  const uint64_t src_span = (uint64_t)(height - 1) * src_stride + width * 2u;
  if (dst_span > INT32_MAX || (src && src_span > INT32_MAX)) return 0;
  /* DMA2D has memcpy semantics, not memmove semantics. */
  if (src && (uint64_t)(uintptr_t)src < (uint64_t)(uintptr_t)dst + dst_span &&
      (uint64_t)(uintptr_t)dst < (uint64_t)(uintptr_t)src + src_span) return 0;
  if (completion == NULL) {
    int init_rc = rtos_init_semaphore_ex(&completion, 1, 0);
    if (init_rc != BK_OK) {
      printf("H2_DMA2D semaphore_init rc=%d\n", init_rc);
      disabled = 1;
      return 0;
    }
    init_rc = bk_dma2d_driver_init();
    if (init_rc != BK_OK) {
      printf("H2_DMA2D driver_init rc=%d\n", init_rc);
      rtos_deinit_semaphore(&completion);
      completion = NULL;
      disabled = 1;
      return 0;
    }
    bk_dma2d_register_int_callback_isr(DMA2D_CFG_ERROR_ISR, transfer_failed, NULL);
    bk_dma2d_register_int_callback_isr(DMA2D_TRANS_ERROR_ISR, transfer_failed, NULL);
    bk_dma2d_register_int_callback_isr(DMA2D_TRANS_COMPLETE_ISR, transfer_done, NULL);
    bk_dma2d_int_enable(DMA2D_CFG_ERROR | DMA2D_TRANS_ERROR | DMA2D_TRANS_COMPLETE, 1);
  }
  transfer_error = 0;
  const size_t dst_bytes = (size_t)(height - 1) * dst_stride + width * 2u;
  sync_cache(dst, (long)dst_bytes);
  if (src != NULL) {
    sync_cache((void *)src, (long)((size_t)(height - 1) * src_stride + width * 2u));
    dma2d_memcpy_pfc_t copy = {0};
    copy.input_addr = (void *)src;
    copy.output_addr = dst;
    /* BK's raw M2M mode forces 32-bit output in dma2d_hal_init(). Use
     * PFC even for identical formats so RGB565 width and stride stay 16-bit. */
    copy.mode = DMA2D_M2M_PFC;
    copy.input_alpha = 0xff;
    copy.input_color_mode = DMA2D_INPUT_RGB565;
    copy.output_color_mode = DMA2D_OUTPUT_RGB565;
    copy.src_pixel_byte = TWO_BYTES;
    copy.dst_pixel_byte = TWO_BYTES;
    copy.src_frame_width = src_stride / 2;
    copy.src_frame_height = height;
    copy.dst_frame_width = dst_stride / 2;
    copy.dst_frame_height = height;
    copy.dma2d_width = width;
    copy.dma2d_height = height;
    bk_dma2d_memcpy_or_pixel_convert(&copy);
  } else {
    dma2d_fill_t fill = {0};
    fill.frameaddr = dst;
    fill.frame_xsize = dst_stride / 2;
    fill.frame_ysize = height;
    fill.width = width;
    fill.height = height;
    fill.color_format = DMA2D_OUTPUT_RGB565;
    fill.pixel_byte = TWO_BYTES;
    fill.color = color;
    dma2d_fill(&fill);
  }
  bk_dma2d_start_transfer();
  if (rtos_get_semaphore(&completion, 100) != BK_OK || transfer_error) {
    bk_dma2d_stop_transfer();
    bk_dma2d_int_enable(DMA2D_CFG_ERROR | DMA2D_TRANS_ERROR | DMA2D_TRANS_COMPLETE, 0);
    bk_dma2d_driver_deinit();
    disabled = 1;
    sync_cache(dst, (long)dst_bytes);
    printf("H2_DMA2D disabled after transfer failure\r\n");
    return 0;
  }
  sync_cache(dst, (long)dst_bytes);
  const unsigned operation_bit = src ? 2u : 1u;
  if ((verified_operations & operation_bit) == 0) {
    for (int32_t y = 0; y < height; ++y) {
      const uint16_t *actual = (const uint16_t *)((const uint8_t *)dst + y * dst_stride);
      const uint16_t *expected = src ?
          (const uint16_t *)((const uint8_t *)src + y * src_stride) : NULL;
      for (int32_t x = 0; x < width; ++x) {
        if (actual[x] != (src ? expected[x] : color)) {
          disabled = 1;
          printf("H2_DMA2D verify_failed operation=%s x=%ld y=%ld actual=%04x expected=%04x src=%p dst=%p\r\n",
                    src ? "copy" : "fill", (long)x, (long)y,
                    (unsigned)actual[x], (unsigned)(src ? expected[x] : color), src, dst);
          return 0;
        }
      }
    }
    verified_operations |= operation_bit;
    printf("H2_DMA2D verified operation=%s pixels=%lu\r\n",
              src ? "copy" : "fill", (unsigned long)width * height);
  }
  if (++completed == 1 || completed % 1000 == 0) {
    printf("H2_DMA2D completed=%u operation=%s\r\n", completed, src ? "copy" : "fill");
  }
  return 1;
}

int h2_bk_dma2d_suspend(void) {
  if (completion == NULL) return BK_OK;
  bk_dma2d_int_enable(DMA2D_CFG_ERROR | DMA2D_TRANS_ERROR | DMA2D_TRANS_COMPLETE, 0);
  int rc = bk_dma2d_driver_deinit();
  if (rc != BK_OK) return rc;
  rc = rtos_deinit_semaphore(&completion);
  if (rc == BK_OK) completion = NULL;
  return rc;
}
