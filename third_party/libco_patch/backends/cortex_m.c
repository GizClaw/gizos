#define LIBCO_C
#include "libco.h"
#include "settings.h"
#include "valgrind.h"

#include <stdint.h>

#if !defined(__arm__) || !defined(__thumb__) ||                                \
    !defined(__ARM_ARCH_PROFILE) || __ARM_ARCH_PROFILE != 'M'
#error "h2_libco_cortex_m.c requires a 32-bit Arm Thumb M-profile target"
#endif

#if UINTPTR_MAX != UINT32_MAX
#error "h2_libco_cortex_m.c requires 32-bit pointers"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
#define H2_LIBCO_CORTEX_M_HAS_PSPLIM 1
#else
#define H2_LIBCO_CORTEX_M_HAS_PSPLIM 0
#endif

#if defined(__ARM_FP) && __ARM_FP != 0
#define H2_LIBCO_CORTEX_M_FP_WORDS 16u
#else
#define H2_LIBCO_CORTEX_M_FP_WORDS 0u
#endif

#define H2_LIBCO_CORTEX_M_CORE_WORDS 10u
#define H2_LIBCO_CORTEX_M_LIMIT_WORDS (H2_LIBCO_CORTEX_M_HAS_PSPLIM ? 2u : 0u)
#define H2_LIBCO_CORTEX_M_CONTEXT_WORDS                                        \
  (H2_LIBCO_CORTEX_M_CORE_WORDS + H2_LIBCO_CORTEX_M_LIMIT_WORDS +              \
   H2_LIBCO_CORTEX_M_FP_WORDS)

/* The executor is restricted to one Board-owned AP task. */
static uintptr_t co_active_buffer[64];
static cothread_t co_active_handle;

__attribute__((naked, noinline)) static void
co_swap_function(cothread_t next __attribute__((unused)),
                 cothread_t previous __attribute__((unused))) {
  /* Preserve the AAPCS callee-saved core and optional FP registers. */
  __asm__ volatile(".syntax unified\n"
                   "stmia r1!, {r4-r11}\n"
                   "mov r2, sp\n"
                   "str r2, [r1], #4\n"
                   "str lr, [r1], #4\n"
#if H2_LIBCO_CORTEX_M_HAS_PSPLIM
                   "mrs r2, psplim\n"
                   "str r2, [r1], #4\n"
                   "mrs r2, primask\n"
                   "str r2, [r1], #4\n"
#endif
#if defined(__ARM_FP) && __ARM_FP != 0
                   "vstmia r1!, {s16-s31}\n"
#endif
                   "ldmia r0!, {r4-r11}\n"
                   "ldr r2, [r0], #4\n"
                   "ldr r3, [r0], #4\n"
#if H2_LIBCO_CORTEX_M_HAS_PSPLIM
                   "ldr r12, [r0], #4\n"
                   "ldr r1, [r0], #4\n"
                   "cpsid i\n"
                   "eor lr, lr, lr\n"
                   "msr psplim, lr\n"
                   "mov sp, r2\n"
                   "msr psplim, r12\n"
#else
                   "mov sp, r2\n"
#endif
#if defined(__ARM_FP) && __ARM_FP != 0
                   "vldmia r0!, {s16-s31}\n"
#endif
#if H2_LIBCO_CORTEX_M_HAS_PSPLIM
                   "msr primask, r1\n"
#endif
                   "bx r3\n");
}

cothread_t co_active(void) {
  if (co_active_handle == NULL) {
    co_active_handle = &co_active_buffer;
  }
  return co_active_handle;
}

cothread_t co_derive(void *memory, unsigned int size,
                     void (*entrypoint)(void)) {
  uintptr_t *handle = (uintptr_t *)memory;

  if (co_active_handle == NULL) {
    co_active_handle = &co_active_buffer;
  }
  (void)VALGRIND_STACK_REGISTER(memory, (uint8_t *)memory + size);
  if (handle != NULL &&
      size >= H2_LIBCO_CORTEX_M_CONTEXT_WORDS * sizeof(*handle)) {
    uintptr_t stack_top = ((uintptr_t)handle + size) & ~(uintptr_t)15u;
    handle[8] = stack_top;
    handle[9] = (uintptr_t)entrypoint;
#if H2_LIBCO_CORTEX_M_HAS_PSPLIM
    handle[10] = (uintptr_t)(handle + H2_LIBCO_CORTEX_M_CONTEXT_WORDS);
    handle[11] = 0u;
#endif
  } else {
    handle = NULL;
  }
  return handle;
}

cothread_t co_create(unsigned int size, void (*entrypoint)(void)) {
  void *memory = LIBCO_MALLOC(size);
  return memory == NULL ? NULL : co_derive(memory, size, entrypoint);
}

void co_delete(cothread_t handle) { LIBCO_FREE(handle); }

void co_switch(cothread_t handle) {
  cothread_t previous = co_active_handle;
  co_active_handle = handle;
  co_swap_function(handle, previous);
}

int co_serializable(void) { return 1; }

#ifdef __cplusplus
}
#endif
