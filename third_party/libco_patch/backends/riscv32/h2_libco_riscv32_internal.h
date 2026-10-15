#ifndef H2_LIBCO_RISCV32_INTERNAL_H
#define H2_LIBCO_RISCV32_INTERNAL_H

#define H2_LIBCO_RISCV32_SP 0
#define H2_LIBCO_RISCV32_RA 4
#define H2_LIBCO_RISCV32_S0 8
#define H2_LIBCO_RISCV32_FP_S0 56
#define H2_LIBCO_RISCV32_STACK_MIN 104
#define H2_LIBCO_RISCV32_STACK_MAX 108
#define H2_LIBCO_RISCV32_SWITCH_TOKEN 112
#define H2_LIBCO_RISCV32_CONTEXT_SIZE 128
#define H2_LIBCO_RISCV32_STACK_ALIGNMENT 16

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct h2_libco_riscv32_context {
  uint32_t sp;
  uint32_t ra;
  uint32_t s[12];
  uint32_t fs[12];
  uint32_t stack_min;
  uint32_t stack_max;
  uint32_t switch_token;
  uint32_t reserved[3];
} h2_libco_riscv32_context_t;

/* Default hooks are no-ops. Enter receives the previous range destinations
 * and spans the SP change; exit runs on the destination stack. */
uint32_t h2_libco_riscv32_stack_prepare(uint32_t stack_min, uint32_t stack_max);
uint32_t h2_libco_riscv32_stack_switch_enter(
    uint32_t next_min, uint32_t next_max,
    uint32_t *previous_min, uint32_t *previous_max);
void h2_libco_riscv32_stack_switch_exit(uint32_t token);

void h2_libco_riscv32_swap(h2_libco_riscv32_context_t *next,
                           h2_libco_riscv32_context_t *previous);

#endif

#endif
