#define LIBCO_C
#include "libco.h"
#include "settings.h"
#include "valgrind.h"

#include "h2_libco_riscv32_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__riscv) || __riscv_xlen != 32
#error "h2_libco RISC-V backend requires RV32"
#endif

_Static_assert(sizeof(void *) == sizeof(uint32_t),
               "RV32 libco context requires 32-bit pointers");
_Static_assert(sizeof(h2_libco_riscv32_context_t) ==
                   H2_LIBCO_RISCV32_CONTEXT_SIZE,
               "RV32 context size must match assembly");
_Static_assert(offsetof(h2_libco_riscv32_context_t, sp) ==
                   H2_LIBCO_RISCV32_SP,
               "RV32 SP offset must match assembly");
_Static_assert(offsetof(h2_libco_riscv32_context_t, ra) ==
                   H2_LIBCO_RISCV32_RA,
               "RV32 RA offset must match assembly");
_Static_assert(offsetof(h2_libco_riscv32_context_t, s) ==
                   H2_LIBCO_RISCV32_S0,
               "RV32 S0 offset must match assembly");
_Static_assert(offsetof(h2_libco_riscv32_context_t, fs) ==
                   H2_LIBCO_RISCV32_FP_S0,
               "RV32 FS0 offset must match assembly");

/* A platform can reserve native metadata above the coroutine's usable SP. */
__attribute__((weak, noinline)) uint32_t h2_libco_riscv32_stack_prepare(
    uint32_t stack_min, uint32_t stack_max) {
  (void)stack_min;
  return stack_max;
}

/* Strong platform implementations may update RTOS stack accounting/guards.
 * Other RV32 consumers preserve their existing execution behavior. */
__attribute__((weak, noinline)) uint32_t h2_libco_riscv32_stack_switch_enter(
    uint32_t next_min, uint32_t next_max,
    uint32_t *previous_min, uint32_t *previous_max) {
  (void)next_min;
  (void)next_max;
  (void)previous_min;
  (void)previous_max;
  return 0;
}
__attribute__((weak, noinline)) void h2_libco_riscv32_stack_switch_exit(uint32_t token) {
  (void)token;
}

_Static_assert(offsetof(h2_libco_riscv32_context_t, stack_min) ==
                   H2_LIBCO_RISCV32_STACK_MIN, "RV32 stack bounds offset");
_Static_assert(offsetof(h2_libco_riscv32_context_t, switch_token) ==
                   H2_LIBCO_RISCV32_SWITCH_TOKEN, "RV32 switch token offset");

static h2_libco_riscv32_context_t s_root_context;
static h2_libco_riscv32_context_t *s_active_context;

cothread_t co_active(void) {
  if (s_active_context == NULL) {
    memset(&s_root_context, 0, sizeof(s_root_context));
    s_active_context = &s_root_context;
  }
  return s_active_context;
}

cothread_t co_derive(void *memory, unsigned int size,
                     void (*entrypoint)(void)) {
  uintptr_t base = (uintptr_t)memory;
  uintptr_t top;
  h2_libco_riscv32_context_t *context;

  if (memory == NULL || entrypoint == NULL ||
      size < H2_LIBCO_RISCV32_CONTEXT_SIZE + 128u) {
    return NULL;
  }
  top = (base + size) & ~(uintptr_t)(H2_LIBCO_RISCV32_STACK_ALIGNMENT - 1u);
  if (top <= base + H2_LIBCO_RISCV32_CONTEXT_SIZE + 64u) {
    return NULL;
  }
  context = memory;
  memset(context, 0, sizeof(*context));
  context->stack_min = (uint32_t)(base + H2_LIBCO_RISCV32_CONTEXT_SIZE);
  context->stack_max = (uint32_t)top;
  /* ESP's software canary/high-watermark checks use the active stack range. */
  memset((void *)(uintptr_t)context->stack_min, 0xa5,
         top - context->stack_min);
  context->sp = h2_libco_riscv32_stack_prepare(context->stack_min, context->stack_max);
  if (context->sp <= context->stack_min + 64u || context->sp > context->stack_max)
    return NULL;
  context->ra = (uint32_t)(uintptr_t)entrypoint;
  (void)VALGRIND_STACK_REGISTER(memory, (uint8_t *)memory + size);
  return context;
}

cothread_t co_create(unsigned int size, void (*entrypoint)(void)) {
  void *memory = LIBCO_MALLOC(size);
  cothread_t thread = co_derive(memory, size, entrypoint);
  if (thread == NULL) {
    LIBCO_FREE(memory);
  }
  return thread;
}

void co_delete(cothread_t handle) { LIBCO_FREE(handle); }

void co_switch(cothread_t handle) {
  h2_libco_riscv32_context_t *next = handle;
  h2_libco_riscv32_context_t *previous;
  if (next == NULL || next == s_active_context) {
    abort();
  }
  previous = s_active_context;
  s_active_context = next;
  h2_libco_riscv32_swap(next, previous);
}

int co_serializable(void) { return 1; }
