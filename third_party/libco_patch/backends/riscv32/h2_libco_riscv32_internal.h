#ifndef H2_LIBCO_RISCV32_INTERNAL_H
#define H2_LIBCO_RISCV32_INTERNAL_H

#define H2_LIBCO_RISCV32_SP 0
#define H2_LIBCO_RISCV32_RA 4
#define H2_LIBCO_RISCV32_S0 8
#define H2_LIBCO_RISCV32_FP_S0 56
#define H2_LIBCO_RISCV32_CONTEXT_SIZE 104
#define H2_LIBCO_RISCV32_STACK_ALIGNMENT 16

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct h2_libco_riscv32_context {
  uint32_t sp;
  uint32_t ra;
  uint32_t s[12];
  uint32_t fs[12];
} h2_libco_riscv32_context_t;

void h2_libco_riscv32_swap(h2_libco_riscv32_context_t *next,
                           h2_libco_riscv32_context_t *previous);

#endif

#endif
