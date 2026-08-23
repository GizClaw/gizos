#include "h2_libco_riscv32_internal.h"

#if !defined(__riscv) || __riscv_xlen != 32
#error "h2_libco RISC-V backend requires RV32"
#endif

    .text
    .align 2
    .global h2_libco_riscv32_swap
    .type h2_libco_riscv32_swap,@function
h2_libco_riscv32_swap:
    sw sp, H2_LIBCO_RISCV32_SP(a1)
    sw ra, H2_LIBCO_RISCV32_RA(a1)
    sw s0, H2_LIBCO_RISCV32_S0 + 0(a1)
    sw s1, H2_LIBCO_RISCV32_S0 + 4(a1)
    sw s2, H2_LIBCO_RISCV32_S0 + 8(a1)
    sw s3, H2_LIBCO_RISCV32_S0 + 12(a1)
    sw s4, H2_LIBCO_RISCV32_S0 + 16(a1)
    sw s5, H2_LIBCO_RISCV32_S0 + 20(a1)
    sw s6, H2_LIBCO_RISCV32_S0 + 24(a1)
    sw s7, H2_LIBCO_RISCV32_S0 + 28(a1)
    sw s8, H2_LIBCO_RISCV32_S0 + 32(a1)
    sw s9, H2_LIBCO_RISCV32_S0 + 36(a1)
    sw s10, H2_LIBCO_RISCV32_S0 + 40(a1)
    sw s11, H2_LIBCO_RISCV32_S0 + 44(a1)
#if defined(__riscv_flen) && __riscv_flen >= 32
    fsw fs0, H2_LIBCO_RISCV32_FP_S0 + 0(a1)
    fsw fs1, H2_LIBCO_RISCV32_FP_S0 + 4(a1)
    fsw fs2, H2_LIBCO_RISCV32_FP_S0 + 8(a1)
    fsw fs3, H2_LIBCO_RISCV32_FP_S0 + 12(a1)
    fsw fs4, H2_LIBCO_RISCV32_FP_S0 + 16(a1)
    fsw fs5, H2_LIBCO_RISCV32_FP_S0 + 20(a1)
    fsw fs6, H2_LIBCO_RISCV32_FP_S0 + 24(a1)
    fsw fs7, H2_LIBCO_RISCV32_FP_S0 + 28(a1)
    fsw fs8, H2_LIBCO_RISCV32_FP_S0 + 32(a1)
    fsw fs9, H2_LIBCO_RISCV32_FP_S0 + 36(a1)
    fsw fs10, H2_LIBCO_RISCV32_FP_S0 + 40(a1)
    fsw fs11, H2_LIBCO_RISCV32_FP_S0 + 44(a1)
#endif

    lw sp, H2_LIBCO_RISCV32_SP(a0)
    lw ra, H2_LIBCO_RISCV32_RA(a0)
    lw s0, H2_LIBCO_RISCV32_S0 + 0(a0)
    lw s1, H2_LIBCO_RISCV32_S0 + 4(a0)
    lw s2, H2_LIBCO_RISCV32_S0 + 8(a0)
    lw s3, H2_LIBCO_RISCV32_S0 + 12(a0)
    lw s4, H2_LIBCO_RISCV32_S0 + 16(a0)
    lw s5, H2_LIBCO_RISCV32_S0 + 20(a0)
    lw s6, H2_LIBCO_RISCV32_S0 + 24(a0)
    lw s7, H2_LIBCO_RISCV32_S0 + 28(a0)
    lw s8, H2_LIBCO_RISCV32_S0 + 32(a0)
    lw s9, H2_LIBCO_RISCV32_S0 + 36(a0)
    lw s10, H2_LIBCO_RISCV32_S0 + 40(a0)
    lw s11, H2_LIBCO_RISCV32_S0 + 44(a0)
#if defined(__riscv_flen) && __riscv_flen >= 32
    flw fs0, H2_LIBCO_RISCV32_FP_S0 + 0(a0)
    flw fs1, H2_LIBCO_RISCV32_FP_S0 + 4(a0)
    flw fs2, H2_LIBCO_RISCV32_FP_S0 + 8(a0)
    flw fs3, H2_LIBCO_RISCV32_FP_S0 + 12(a0)
    flw fs4, H2_LIBCO_RISCV32_FP_S0 + 16(a0)
    flw fs5, H2_LIBCO_RISCV32_FP_S0 + 20(a0)
    flw fs6, H2_LIBCO_RISCV32_FP_S0 + 24(a0)
    flw fs7, H2_LIBCO_RISCV32_FP_S0 + 28(a0)
    flw fs8, H2_LIBCO_RISCV32_FP_S0 + 32(a0)
    flw fs9, H2_LIBCO_RISCV32_FP_S0 + 36(a0)
    flw fs10, H2_LIBCO_RISCV32_FP_S0 + 40(a0)
    flw fs11, H2_LIBCO_RISCV32_FP_S0 + 44(a0)
#endif
    ret

    .size h2_libco_riscv32_swap, . - h2_libco_riscv32_swap
