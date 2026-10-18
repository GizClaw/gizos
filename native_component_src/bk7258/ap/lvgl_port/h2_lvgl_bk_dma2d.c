#include "h2_lvgl_bk_dma2d.h"
#include "h2_lvgl_bk_power.h"

#include <common/bk_include.h>

#if CONFIG_DMA2D
#include <armstar.h>
#include <common/bk_assert.h>
#include <driver/dma2d.h>
#include <modules/pm.h>
#include <os/os.h>
#include <limits.h>

/* The BK LVGL config has one draw unit. This component exclusively owns
 * DMA2D; SPI transfers use the separate general-purpose DMA engine. */
static beken_semaphore_t completion;
static volatile int transfer_error;
static int initialized;
static int partial_init_power;
static int disabled;
static int last_was_fill;

static void sync_cache(void *address, long bytes) {
    if (SCB->CLIDR & SCB_CLIDR_DC_Msk)
        SCB_CleanInvalidateDCache_by_Addr(address, bytes);
}

static void transfer_done(void *user) {
    (void)user;
    (void)rtos_set_semaphore(&completion);
}

static void transfer_failed(void *user) {
    (void)user;
    transfer_error = 1;
    (void)rtos_set_semaphore(&completion);
}

static int release_resources(void) {
    if (initialized) {
        /* Retain the driver and semaphore if stop/masking fails. A late ISR
         * must never observe freed completion storage. */
        int rc = bk_dma2d_stop_transfer();
        if (rc != BK_OK) return rc;
        rc = bk_dma2d_int_enable(
            DMA2D_CFG_ERROR | DMA2D_TRANS_ERROR | DMA2D_TRANS_COMPLETE, 0);
        if (rc != BK_OK) return rc;
        bk_dma2d_soft_reset();
        if (bk_dma2d_is_transfer_busy()) return BK_FAIL;
        rc = bk_dma2d_driver_deinit();
        if (rc != BK_OK) return rc;
        initialized = 0;
    }
    if (partial_init_power) {
        int rc = bk_dma2d_driver_deinit();
        if (rc != BK_OK) return rc;
        rc = bk_pm_module_vote_power_ctrl(
            PM_POWER_SUB_MODULE_NAME_VIDP_DMA2D, PM_POWER_MODULE_STATE_OFF);
        if (rc != BK_OK) return rc;
        partial_init_power = 0;
    }
    if (completion != NULL) {
        int rc = rtos_deinit_semaphore(&completion);
        if (rc != BK_OK) return rc;
        completion = NULL;
    }
    last_was_fill = 0;
    return BK_OK;
}

static int disable_acceleration(void) {
    disabled = 1;
    int rc = release_resources();
    if (rc != BK_OK && initialized) {
        /* A failed stop can leave DMA writing the caller's buffer. Force a
         * logic reset and require idle readback before allowing CPU redraw.
         * Failed masking retains IRQ, driver and semaphore until retry. */
        bk_dma2d_soft_reset();
        if (bk_dma2d_is_transfer_busy()) {
            BK_ASSERT(0);
            /* ASSERT_IGNORE builds must also retain buffer ownership instead
             * of returning to LVGL while hardware can still access it. */
            for (;;) rtos_delay_milliseconds(1000);
        }
    }
    return 0;
}

static int initialize(void) {
    if (initialized) return 1;
    if (rtos_init_semaphore_ex(&completion, 1, 0) != BK_OK)
        return disable_acceleration();
    if (bk_dma2d_driver_init() != BK_OK) {
        /* The pinned SDK votes power before its last fallible init step. */
        partial_init_power = 1;
        return disable_acceleration();
    }
    initialized = 1;
    if (bk_dma2d_register_int_callback_isr(
            DMA2D_CFG_ERROR_ISR, transfer_failed, NULL) != BK_OK ||
        bk_dma2d_register_int_callback_isr(
            DMA2D_TRANS_ERROR_ISR, transfer_failed, NULL) != BK_OK ||
        bk_dma2d_register_int_callback_isr(
            DMA2D_TRANS_COMPLETE_ISR, transfer_done, NULL) != BK_OK ||
        bk_dma2d_int_enable(
            DMA2D_CFG_ERROR | DMA2D_TRANS_ERROR | DMA2D_TRANS_COMPLETE, 1) != BK_OK)
        return disable_acceleration();
    return 1;
}

int h2_bk_dma2d_rgb565(void *dst, const void *src, int32_t width,
                     int32_t height, uint32_t dst_stride,
                     uint32_t src_stride, uint16_t color) {
    if (disabled || dst == NULL || width <= 0 || height <= 0 ||
        width > 16383 || height > 65535 || (uint64_t)width * height < 128 ||
        dst_stride % 2 || dst_stride / 2 < (uint32_t)width ||
        dst_stride / 2 > 65535 ||
        dst_stride / 2 - (uint32_t)width > 16383 || ((uintptr_t)dst & 1) ||
        (src && (src_stride % 2 || src_stride / 2 < (uint32_t)width ||
                 src_stride / 2 > 65535 ||
                 src_stride / 2 - (uint32_t)width > 16383 || ((uintptr_t)src & 1))))
        return 0;
    const uint64_t dst_bytes = (uint64_t)(height - 1) * dst_stride + width * 2u;
    const uint64_t src_bytes = (uint64_t)(height - 1) * src_stride + width * 2u;
    if (dst_bytes > INT32_MAX || (src && src_bytes > INT32_MAX) ||
        (uintptr_t)dst > UINTPTR_MAX - dst_bytes ||
        (src && (uintptr_t)src > UINTPTR_MAX - src_bytes)) return 0;
    /* The engine has memcpy semantics; overlapping ranges stay on CPU. */
    if (src && (uintptr_t)src < (uintptr_t)dst + dst_bytes &&
        (uintptr_t)dst < (uintptr_t)src + src_bytes) return 0;
    if (!initialize()) return 0;
    transfer_error = 0;
    sync_cache(dst, (long)dst_bytes);
    if (src != NULL) {
        sync_cache((void *)src, (long)src_bytes);
        dma2d_memcpy_pfc_t copy = {0};
        copy.input_addr = (void *)src;
        copy.output_addr = dst;
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
        /* Pinned BK hardware requires new M2M configuration before resetting
         * the retained internal R2M state. Consecutive copies need no reset. */
        if (last_was_fill) bk_dma2d_soft_reset();
        last_was_fill = 0;
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
        if (dma2d_fill(&fill) != BK_OK) return disable_acceleration();
        last_was_fill = 1;
    }
    if (bk_dma2d_start_transfer() != BK_OK ||
        rtos_get_semaphore(&completion, 100) != BK_OK || transfer_error ||
        bk_dma2d_is_transfer_busy()) {
        (void)disable_acceleration();
        sync_cache(dst, (long)dst_bytes);
        return 0;
    }
    sync_cache(dst, (long)dst_bytes);
    return 1;
}

int h2_bk_dma2d_suspend(void) {
    disabled = 1;
    int rc = release_resources();
    if (rc == BK_OK) {
        disabled = 0;
        transfer_error = 0;
    }
    return rc;
}
#else
int h2_bk_dma2d_rgb565(void *dst, const void *src, int32_t width,
                     int32_t height, uint32_t dst_stride,
                     uint32_t src_stride, uint16_t color) {
    (void)dst;
    (void)src;
    (void)width;
    (void)height;
    (void)dst_stride;
    (void)src_stride;
    (void)color;
    return 0;
}

int h2_bk_dma2d_suspend(void) { return BK_OK; }
#endif
