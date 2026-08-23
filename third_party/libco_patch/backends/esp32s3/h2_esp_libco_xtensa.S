#include "h2_esp_libco_xtensa_internal.h"

#include <xtensa/coreasm.h>
#include <xtensa/corebits.h>
#include <xtensa/xtruntime.h>
#include "xt_asm_utils.h"
#include "xt_instr_macros.h"

#if !defined(__XTENSA__)
#error "h2_libco Xtensa backend requires an Xtensa target"
#endif

#ifdef __XTENSA_CALL0_ABI__
#error "h2_libco Xtensa backend requires the ESP32-S3 windowed ABI"
#endif

    .text
    .align 4
    .extern xthal_window_spill_nw
    .global h2_esp_libco_xtensa_swap
    .type h2_esp_libco_xtensa_swap,@function
h2_esp_libco_xtensa_swap:
    /* Offsets 48..63 are the caller base-save area used by window spill. */
    entry   a1, 64

    s32i    a0, a1, 0
    s32i    a2, a1, 4
    s32i    a3, a1, 8
    rsr     a0, SAR
    s32i    a0, a1, 12
    rsr     a0, LBEG
    s32i    a0, a1, 16
    rsr     a0, LEND
    s32i    a0, a1, 20
    rsr     a0, LCOUNT
    s32i    a0, a1, 24
    rsr     a2, XT_REG_PS
    s32i    a2, a1, 28

    /*
     * Match ESP-IDF's solicited FreeRTOS context switch: disable WOE, then
     * call the no-window HAL spill routine without creating another ABI
     * window.  All older windows end up in this coroutine's own stack.
     */
    movi    a6, ~(PS_WOE_MASK | PS_INTLEVEL_MASK)
    and     a2, a2, a6
    addi    a2, a2, XCHAL_EXCM_LEVEL
    wsr     a2, XT_REG_PS
    rsync
    call0   xthal_window_spill_nw
    l32i    a2, a1, 28
    wsr     a2, XT_REG_PS

    /*
     * Like the ESP-IDF solicited task switch, leave WINDOWBASE/WINDOWSTART in
     * the live normalized mapping after spilling every older window.  retw
     * refills the target caller registers from its restored stack.  Restoring
     * another context's raw mapping here would mark unrelated physical
     * register windows valid before their general registers were reloaded.
     */

    l32i    a0, a1, 0
    l32i    a2, a1, 4
    l32i    a3, a1, 8
    s32i    a1, a3, H2_ESP_LIBCO_CTX_SP
    s32i    a0, a3, H2_ESP_LIBCO_CTX_A0
    l32i    a4, a1, 28
    s32i    a4, a3, H2_ESP_LIBCO_CTX_PS
    l32i    a4, a1, 12
    s32i    a4, a3, H2_ESP_LIBCO_CTX_SAR
    l32i    a4, a1, 16
    s32i    a4, a3, H2_ESP_LIBCO_CTX_LBEG
    l32i    a4, a1, 20
    s32i    a4, a3, H2_ESP_LIBCO_CTX_LEND
    l32i    a4, a1, 24
    s32i    a4, a3, H2_ESP_LIBCO_CTX_LCOUNT

    l32i    a4, a2, H2_ESP_LIBCO_CTX_STARTED
    bnez    a4, 2f
    movi    a4, 1
    s32i    a4, a2, H2_ESP_LIBCO_CTX_STARTED
    l32i    a6, a2, H2_ESP_LIBCO_CTX_STACK_TOP
    l32i    a14, a2, H2_ESP_LIBCO_CTX_ENTRYPOINT
    SET_STACK a6, a7, a8
    callx8  a14
    break   0, 0

2:
    mov     a11, a2
    l32i    a2, a11, H2_ESP_LIBCO_CTX_SP
    l32i    a3, a11, H2_ESP_LIBCO_CTX_A0
    l32i    a4, a11, H2_ESP_LIBCO_CTX_PS

    l32i    a5, a11, H2_ESP_LIBCO_CTX_SAR
    wsr     a5, SAR
    l32i    a5, a11, H2_ESP_LIBCO_CTX_LBEG
    wsr     a5, LBEG
    l32i    a5, a11, H2_ESP_LIBCO_CTX_LEND
    wsr     a5, LEND
    l32i    a5, a11, H2_ESP_LIBCO_CTX_LCOUNT
    wsr     a5, LCOUNT

    mov     a1, a2
    mov     a0, a3
    wsr     a4, XT_REG_PS
    rsync
    retw

    .size h2_esp_libco_xtensa_swap, . - h2_esp_libco_xtensa_swap
