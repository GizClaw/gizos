#include "h2_lvgl_bk_dma2d.h"
#include "h2_lvgl_bk_power.h"
#include <armstar.h>
#include <driver/dma2d.h>
#include <modules/pm.h>
#include <os/os.h>

#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

struct test_semaphore { unsigned count; };
test_scb_t test_scb = {1};
static dma2d_isr_t callbacks[DMA2D_ISR_NUM];
static void *callback_users[DMA2D_ISR_NUM];
static dma2d_memcpy_pfc_t pending_copy;
static dma2d_fill_t pending_fill;
static int is_fill, powered, irq_enabled, busy;
static unsigned semaphores, starts, resets, copies, cache_calls, registrations;
static char trace[128];
static size_t trace_length;
static unsigned real_deinit_calls;
static int require_real_deinit;
static int fail_semaphore, fail_driver, fail_registration, fail_enable;
static int fail_fill, fail_start, fail_timeout, fail_isr, fail_busy;
static int fail_driver_deinit, fail_semaphore_deinit;
static int fail_stop, fail_irq_disable, fail_reset_busy;
static jmp_buf assertion_context;
static int assertion_expected, assertion_seen;
void test_sdk_assert(void);
void test_sdk_assert(void) {
    assert(assertion_expected);
    assertion_seen = 1;
    longjmp(assertion_context, 1);
}
void rtos_delay_milliseconds(unsigned milliseconds) {
    (void)milliseconds;
    assert(!"Unquiesced DMA must enter the SDK assertion before sleeping");
}

static void event(char value) {
    if (trace_length < sizeof(trace) - 1) trace[trace_length++] = value;
    trace[trace_length] = 0;
}
int rtos_init_semaphore_ex(beken_semaphore_t *out, unsigned maximum, unsigned initial) {
    assert(maximum == 1 && initial == 0);
    *out = NULL;
    if (fail_semaphore) return BK_FAIL;
    *out = calloc(1, sizeof(**out));
    assert(*out != NULL);
    ++semaphores;
    return BK_OK;
}
int rtos_deinit_semaphore(beken_semaphore_t *semaphore) {
    assert(!powered && !irq_enabled && !busy);
    if (fail_semaphore_deinit) return BK_FAIL;
    assert(*semaphore != NULL && semaphores == 1);
    free(*semaphore);
    *semaphore = NULL;
    --semaphores;
    event('Q');
    return BK_OK;
}
int rtos_set_semaphore(beken_semaphore_t *semaphore) {
    assert(*semaphore != NULL);
    (*semaphore)->count = 1;
    return BK_OK;
}
int rtos_get_semaphore(beken_semaphore_t *semaphore, unsigned timeout_ms) {
    assert(timeout_ms == 100 && *semaphore != NULL);
    if (fail_timeout) return BK_FAIL;
    if (!fail_isr) {
        if (is_fill) {
            uint16_t *output = pending_fill.frameaddr;
            for (unsigned y = 0; y < pending_fill.height; ++y)
                for (unsigned x = 0; x < pending_fill.width; ++x)
                    output[y * pending_fill.frame_xsize + x] = pending_fill.color;
        } else {
            uint16_t *output = pending_copy.output_addr;
            const uint16_t *input = pending_copy.input_addr;
            for (unsigned y = 0; y < pending_copy.dma2d_height; ++y)
                memcpy(output + y * pending_copy.dst_frame_width,
                       input + y * pending_copy.src_frame_width,
                       pending_copy.dma2d_width * sizeof(uint16_t));
        }
    }
    busy = fail_busy;
    const int id = fail_isr ? DMA2D_TRANS_ERROR_ISR : DMA2D_TRANS_COMPLETE_ISR;
    assert(irq_enabled && callbacks[id] != NULL);
    callbacks[id](callback_users[id]);
    assert((*semaphore)->count == 1);
    (*semaphore)->count = 0;
    return BK_OK;
}
int bk_dma2d_driver_init(void) {
    assert(!powered);
    powered = 1;
    memset(callbacks, 0, sizeof(callbacks));
    return fail_driver ? BK_FAIL : BK_OK;
}
int bk_dma2d_driver_deinit(void) {
    if (require_real_deinit) assert(real_deinit_calls > 0);
    assert(!busy && !irq_enabled);
    if (fail_driver_deinit) return BK_FAIL;
    powered = 0;
    memset(callbacks, 0, sizeof(callbacks));
    event('D');
    return BK_OK;
}
int bk_pm_module_vote_power_ctrl(int module, int state) {
    assert(module == PM_POWER_SUB_MODULE_NAME_VIDP_DMA2D);
    assert(state == PM_POWER_MODULE_STATE_OFF);
    powered = 0;
    return BK_OK;
}
int bk_dma2d_register_int_callback_isr(int id, dma2d_isr_t callback, void *user) {
    assert(powered);
    if (++registrations == (unsigned)fail_registration) return BK_FAIL;
    callbacks[id] = callback;
    callback_users[id] = user;
    return BK_OK;
}
int bk_dma2d_int_enable(unsigned mask, bool enable) {
    assert(mask == (DMA2D_CFG_ERROR | DMA2D_TRANS_ERROR | DMA2D_TRANS_COMPLETE));
    if ((enable && fail_enable) || (!enable && fail_irq_disable)) return BK_FAIL;
    irq_enabled = enable;
    return BK_OK;
}
int bk_dma2d_stop_transfer(void) { event('T'); if (fail_stop) return BK_FAIL; busy = 0; return BK_OK; }
int bk_dma2d_start_transfer(void) {
    assert(powered && irq_enabled);
    ++starts;
    event('S');
    if (fail_start) return BK_FAIL;
    busy = 1;
    return BK_OK;
}
void bk_dma2d_soft_reset(void) { if (!fail_reset_busy) busy = 0; ++resets; event('R'); }
bool bk_dma2d_is_transfer_busy(void) { return busy != 0; }
void bk_dma2d_memcpy_or_pixel_convert(dma2d_memcpy_pfc_t *copy) {
    assert(copy->mode == DMA2D_M2M && copy->input_alpha == 0xff);
    assert(copy->input_color_mode == DMA2D_INPUT_RGB565);
    assert(copy->output_color_mode == DMA2D_OUTPUT_RGB565);
    assert(copy->src_pixel_byte == TWO_BYTES && copy->dst_pixel_byte == TWO_BYTES);
    pending_copy = *copy;
    is_fill = 0;
    ++copies;
    event('C');
}
int dma2d_fill(dma2d_fill_t *fill) {
    assert(fill->color_format == DMA2D_OUTPUT_RGB565 && fill->pixel_byte == TWO_BYTES);
    pending_fill = *fill;
    is_fill = 1;
    event('F');
    return fail_fill ? BK_FAIL : BK_OK;
}
void SCB_CleanInvalidateDCache_by_Addr(void *address, long bytes) {
    assert(address != NULL && bytes > 0);
    ++cache_calls;
}
void __wrap_lv_deinit(void);
void __real_lv_deinit(void) { ++real_deinit_calls; }

static void clear_fixture(void) {
    fail_driver_deinit = fail_semaphore_deinit = 0;
    fail_stop = fail_irq_disable = fail_reset_busy = 0;
    assertion_expected = assertion_seen = 0;
    require_real_deinit = 0;
    assert(h2_bk_dma2d_suspend() == BK_OK);
    assert(semaphores == 0 && !powered && !irq_enabled && !busy);
    fail_semaphore = fail_driver = fail_registration = fail_enable = 0;
    fail_fill = fail_start = fail_timeout = fail_isr = fail_busy = 0;
    starts = resets = copies = cache_calls = registrations = real_deinit_calls = 0;
    trace_length = 0;
    trace[0] = 0;
}
static void test_stride_and_transition(void) {
    clear_fixture();
    uint16_t source[8 * 19], output[8 * 23];
    for (unsigned i = 0; i < 8 * 19; ++i) source[i] = (uint16_t)(0x1234 + i * 71);
    for (unsigned i = 0; i < 8 * 23; ++i) output[i] = 0xa55a;
    assert(h2_bk_dma2d_rgb565(output, NULL, 16, 8, 46, 0, 0x5aa5) == 1);
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 23; ++x)
            assert(output[y * 23 + x] == (x < 16 ? 0x5aa5 : 0xa55a));
    assert(h2_bk_dma2d_rgb565(output, source, 16, 8, 46, 38, 0) == 1);
    assert(strstr(trace, "CRS") != NULL && resets == 1);
    assert(h2_bk_dma2d_rgb565(output, source, 16, 8, 46, 38, 0) == 1);
    assert(resets == 1);
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 23; ++x)
            assert(output[y * 23 + x] == (x < 16 ? source[y * 19 + x] : 0xa55a));
    assert(cache_calls > 0 && starts == 3);
    require_real_deinit = 1;
    __wrap_lv_deinit();
    assert(real_deinit_calls == 1 && semaphores == 0 && !powered && !irq_enabled);
    __wrap_lv_deinit();
    assert(real_deinit_calls == 2 && semaphores == 0);
}
static void test_invalid_input(void) {
    clear_fixture();
    uint16_t pixels[512];
    assert(!h2_bk_dma2d_rgb565(NULL, NULL, 16, 8, 32, 0, 0));
    assert(!h2_bk_dma2d_rgb565(pixels, NULL, 0, 8, 32, 0, 0));
    assert(!h2_bk_dma2d_rgb565(pixels, NULL, 16, -1, 32, 0, 0));
    assert(!h2_bk_dma2d_rgb565(pixels, NULL, 1, 1, 2, 0, 0));
    assert(!h2_bk_dma2d_rgb565((char *)pixels + 1, NULL, 16, 8, 32, 0, 0));
    assert(!h2_bk_dma2d_rgb565(pixels, NULL, 16, 8, 31, 0, 0));
    assert(!h2_bk_dma2d_rgb565(pixels, pixels + 1, 16, 8, 32, 32, 0));
    assert(!h2_bk_dma2d_rgb565(pixels, NULL, 16384, 8, 32768, 0, 0));
    assert(!h2_bk_dma2d_rgb565(pixels, NULL, 16, 65536, 32, 0, 0));
    assert(!h2_bk_dma2d_rgb565(pixels, NULL, 16, 8, UINT32_MAX, 0, 0));
    assert(!h2_bk_dma2d_rgb565((void *)(UINTPTR_MAX - 1), NULL, 16, 8, 32, 0, 0));
    assert(starts == 0 && semaphores == 0 && !powered);
}
static void test_failure(int *fault) {
    clear_fixture();
    *fault = 1;
    uint16_t output[128];
    assert(h2_bk_dma2d_rgb565(output, NULL, 16, 8, 32, 0, 0x3344) == 0);
    assert(!powered && !irq_enabled && !busy && semaphores == 0);
    unsigned attempts = starts;
    *fault = 0;
    assert(h2_bk_dma2d_rgb565(output, NULL, 16, 8, 32, 0, 0) == 0);
    assert(starts == attempts);
    assert(h2_bk_dma2d_suspend() == BK_OK);
    assert(h2_bk_dma2d_rgb565(output, NULL, 16, 8, 32, 0, 0x1234) == 1);
    assert(output[0] == 0x1234 && output[127] == 0x1234);
}
static void test_cleanup_retry(void) {
    clear_fixture();
    uint16_t output[128];
    assert(h2_bk_dma2d_rgb565(output, NULL, 16, 8, 32, 0, 1) == 1);
    fail_driver_deinit = 1;
    assert(h2_bk_dma2d_suspend() == BK_FAIL);
    assert(semaphores == 1 && !irq_enabled && !busy);
    fail_driver_deinit = 0;
    fail_semaphore_deinit = 1;
    assert(h2_bk_dma2d_suspend() == BK_FAIL);
    assert(semaphores == 1 && !powered);
    fail_semaphore_deinit = 0;
    assert(h2_bk_dma2d_suspend() == BK_OK);
    assert(semaphores == 0);
}
static void test_failed_quiescence_retains_resources(int *fault) {
    clear_fixture();
    fail_timeout = 1;
    *fault = 1;
    uint16_t output[128];
    assert(h2_bk_dma2d_rgb565(output, NULL, 16, 8, 32, 0, 1) == 0);
    assert(semaphores == 1 && powered && irq_enabled && !busy);
    /* A pending ISR still sees live storage while cleanup is deferred. */
    assert(callbacks[DMA2D_TRANS_COMPLETE_ISR] != NULL);
    callbacks[DMA2D_TRANS_COMPLETE_ISR](callback_users[DMA2D_TRANS_COMPLETE_ISR]);
    assert(h2_bk_dma2d_suspend() == BK_FAIL);
    assert(semaphores == 1 && powered);
    *fault = 0;
    fail_timeout = 0;
    assert(h2_bk_dma2d_suspend() == BK_OK);
    assert(semaphores == 0 && !powered && !irq_enabled);
    assert(h2_bk_dma2d_rgb565(output, NULL, 16, 8, 32, 0, 0x1234) == 1);
}
static void test_unquiesced_dma_never_returns(void) {
    clear_fixture();
    fail_timeout = fail_stop = fail_reset_busy = 1;
    assertion_expected = 1;
    uint16_t output[128];
    if (setjmp(assertion_context) == 0) {
        (void)h2_bk_dma2d_rgb565(output, NULL, 16, 8, 32, 0, 1);
        assert(!"DMA still owns the buffer; CPU fallback must not return");
    }
    assert(assertion_seen && semaphores == 1 && powered && irq_enabled && busy);
    fail_reset_busy = 0;
    busy = 0;
    clear_fixture();
}
int main(void) {
    test_stride_and_transition();
    test_invalid_input();
    int *faults[] = {&fail_semaphore, &fail_driver, &fail_registration, &fail_enable,
                    &fail_fill, &fail_start, &fail_timeout, &fail_isr, &fail_busy};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) test_failure(faults[i]);
    test_cleanup_retry();
    test_failed_quiescence_retains_resources(&fail_stop);
    test_failed_quiescence_retains_resources(&fail_irq_disable);
    test_unquiesced_dma_never_returns();
    clear_fixture();
    return 0;
}
