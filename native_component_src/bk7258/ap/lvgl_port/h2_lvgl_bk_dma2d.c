#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <os/os.h>
#include <os/mem.h>
#include <driver/dma2d.h>
#include <armstar.h>
#include <soc/soc.h>
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

/* Temporary bring-up probe: compare DMA-visible SRAM and PSRAM using
 * nonzero patterns, independently of LVGL and the video producer. */
static int probe_copy_memory(void) {
  uint16_t *regions[2] = {os_malloc(1024), psram_malloc(1024)};
  if (!regions[0] || !regions[1]) {
    os_free(regions[0]);
    os_free(regions[1]);
    printf("H2_DMA2D probe allocation_failed\n");
    return 0;
  }
  for (unsigned mode = 0; mode < 2; ++mode) {
    for (unsigned from = 0; from < 2; ++from) {
      for (unsigned to = 0; to < 2; ++to) {
        uint16_t *input = regions[from] + 16;
        uint16_t *output = regions[to] + 272;
        for (unsigned i = 0; i < 128; ++i) {
          input[i] = (uint16_t)(0x1234u + i * 71u);
          output[i] = (uint16_t)~input[i];
        }
        dma2d_fill_t fill = {0};
        fill.frameaddr = output;
        fill.frame_xsize = fill.width = 16;
        fill.frame_ysize = fill.height = 8;
        fill.color_format = DMA2D_OUTPUT_RGB565;
        fill.pixel_byte = TWO_BYTES;
        fill.color = 0x5aa5;
        sync_cache(output, 256);
        transfer_error = 0;
        dma2d_fill(&fill);
        bk_dma2d_start_transfer();
        if (rtos_get_semaphore(&completion, 100) != BK_OK ||
            transfer_error || bk_dma2d_is_transfer_busy()) {
          bk_dma2d_stop_transfer();
          printf("H2_DMA2D probe fill_stopped\n");
          return 0;
        }
        sync_cache(output, 256);
        printf("H2_DMA2D probe prefill first=%04x expected=5aa5\n", output[0]);
        for (unsigned i = 0; i < 128; ++i) output[i] = (uint16_t)~input[i];
        sync_cache(input, 256);
        sync_cache(output, 256);
        transfer_error = 0;
        dma2d_memcpy_pfc_t copy = {0};
        copy.input_addr = input;
        copy.output_addr = output;
        copy.mode = mode ? DMA2D_M2M_PFC : DMA2D_M2M;
        copy.input_alpha = 0xff;
        copy.input_color_mode = DMA2D_INPUT_RGB565;
        copy.output_color_mode = DMA2D_OUTPUT_RGB565;
        copy.src_pixel_byte = TWO_BYTES;
        copy.dst_pixel_byte = TWO_BYTES;
        copy.src_frame_width = copy.dst_frame_width = 16;
        copy.src_frame_height = copy.dst_frame_height = 8;
        copy.dma2d_width = 16;
        copy.dma2d_height = 8;
        bk_dma2d_memcpy_or_pixel_convert(&copy);
        bk_dma2d_start_transfer();
        int rc = rtos_get_semaphore(&completion, 100);
        if (rc != BK_OK || transfer_error || bk_dma2d_is_transfer_busy()) {
          bk_dma2d_stop_transfer();
          printf("H2_DMA2D probe stopped rc=%d error=%d\n", rc, transfer_error);
          /* Retain these small buffers if bus quiescence is unproven. */
          return 0;
        }
        sync_cache(output, 256);
        unsigned matched = 0;
        while (matched < 128 && output[matched] == input[matched]) ++matched;
        printf("H2_DMA2D probe mode=%u from=%u to=%u src=%p dst=%p matched=%u first=%04x expected=%04x\n",
               mode, from, to, (void *)input, (void *)output, matched,
               (unsigned)output[0], (unsigned)input[0]);
      }
    }
  }
  os_free(regions[0]);
  os_free(regions[1]);
  return 1;
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
    if (!probe_copy_memory()) {
      disabled = 1;
      return 0;
    }
  }
  transfer_error = 0;
  const int trace_copy = src && !(verified_operations & 2u);
  const uint16_t first_source = src ? *(const uint16_t *)src : 0;
  if (trace_copy) {
    printf("H2_DMA2D copy_begin width=%ld height=%ld src_stride=%lu dst_stride=%lu first=%04x\n",
           (long)width, (long)height, (unsigned long)src_stride,
           (unsigned long)dst_stride, (unsigned)first_source);
  }
  const size_t dst_bytes = (size_t)(height - 1) * dst_stride + width * 2u;
  const unsigned operation_bit = src ? 2u : 1u;
  int reset_retry = 0;
configure_transfer:
  /* A zero-filled target cannot prove that a black fill wrote anything.
   * Seed only the first checked operation with the opposite pixel values.
   * A failed check returns to LVGL's software path, which redraws the area. */
  if ((verified_operations & operation_bit) == 0) {
    for (int32_t y = 0; y < height; ++y) {
      uint16_t *target = (uint16_t *)((uint8_t *)dst + y * dst_stride);
      const uint16_t *input = src ?
          (const uint16_t *)((const uint8_t *)src + y * src_stride) : NULL;
      for (int32_t x = 0; x < width; ++x) {
        target[x] = (uint16_t)~(src ? input[x] : color);
      }
    }
  }
  sync_cache(dst, (long)dst_bytes);
  if (src != NULL) {
    sync_cache((void *)src, (long)((size_t)(height - 1) * src_stride + width * 2u));
    dma2d_memcpy_pfc_t copy = {0};
    copy.input_addr = (void *)src;
    copy.output_addr = dst;
    /* Match the SDK LVGL RGB565 copy path. The first transfer is checked
     * against the source before this path is allowed to remain enabled. */
    copy.mode = DMA2D_M2M;
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
    if (trace_copy) {
      /* BK7258 register offsets from the SDK DMA2D register map. */
      printf("H2_DMA2D registers control=%08lx src=%08lx dst=%08lx input=%08lx output=%08lx size=%08lx seeded=%04x\n",
             (unsigned long)REG_READ(SOC_DMA2D_REG_BASE + 0x04u * 4u),
             (unsigned long)REG_READ(SOC_DMA2D_REG_BASE + 0x07u * 4u),
             (unsigned long)REG_READ(SOC_DMA2D_REG_BASE + 0x13u * 4u),
             (unsigned long)REG_READ(SOC_DMA2D_REG_BASE + 0x0bu * 4u),
             (unsigned long)REG_READ(SOC_DMA2D_REG_BASE + 0x11u * 4u),
             (unsigned long)REG_READ(SOC_DMA2D_REG_BASE + 0x15u * 4u),
             (unsigned)*(const uint16_t *)dst);
    }
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
  if (trace_copy) {
    printf("H2_DMA2D copy_end busy=%u status=%lu source_before=%04x source_after=%04x actual=%04x\n",
           (unsigned)bk_dma2d_is_transfer_busy(),
           (unsigned long)bk_dma2d_int_status_get(), (unsigned)first_source,
           (unsigned)*(const uint16_t *)src, (unsigned)*(const uint16_t *)dst);
  }
  if (trace_copy && !reset_retry && *(const uint16_t *)dst != first_source) {
    printf("H2_DMA2D diagnostic reset_retry before=%04x expected=%04x\n",
           (unsigned)*(const uint16_t *)dst, (unsigned)first_source);
    bk_dma2d_soft_reset();
    bk_dma2d_int_enable(DMA2D_CFG_ERROR | DMA2D_TRANS_ERROR | DMA2D_TRANS_COMPLETE, 1);
    reset_retry = 1;
    goto configure_transfer;
  }
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
