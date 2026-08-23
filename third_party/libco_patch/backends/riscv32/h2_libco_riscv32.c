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
  context->sp = (uint32_t)top;
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
