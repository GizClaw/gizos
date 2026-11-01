/* Temporary CP vector-entry observation. The original handlers remain intact.
 * The SRAM entry restores registers, flags, MSP and EXC_RETURN before tail
 * branching to the original handler. No interrupt is acknowledged here. */
#include "h2_bk_pm_observer.h"

#include "arch_interrupt.h"
#include "armstar.h"
#include "components/log.h"

_Static_assert(16u + INT_ID_MAX == H2_BK_PM_TRACE_VECTOR_COUNT,
               "revalidate the diagnostic vector table for this SDK");
_Static_assert(sizeof(void*) == sizeof(uint32_t),
               "CP vector words require a 32-bit target");

static uint32_t s_original_vectors[H2_BK_PM_TRACE_VECTOR_COUNT];
static uint32_t s_trace_vectors[H2_BK_PM_TRACE_VECTOR_COUNT]
    __attribute__((aligned(512)));
static uint32_t s_vectors_installed;

__attribute__((section(".iram"), noinline, used)) static uint32_t
h2_bk_trace_vector_target(uint32_t vector, uint32_t exc_return) {
  if (vector >= 2u && vector < H2_BK_PM_TRACE_VECTOR_COUNT &&
      s_original_vectors[vector] != 0u) {
    h2_bk_pm_trace_event(H2_BK_PM_TRACE_VECTOR_BASE + vector, exc_return);
    return s_original_vectors[vector];
  }
  /* Only populated entries are redirected, so this is a defensive fallback. */
  return s_original_vectors[3];
}

__attribute__((naked, section(".iram"), noinline, used)) static void
h2_bk_trace_vector_entry(void) {
  __asm volatile(
      "sub sp, sp, #4\n"
      "push {r0-r4, r12, lr}\n"
      "mov r0, sp\n"
      "bic r1, r0, #7\n"
      "mov sp, r1\n"
      "mrs r2, apsr\n"
      "push {r0, r2}\n"
      "mrs r0, ipsr\n"
      "mov r1, lr\n"
      "bl h2_bk_trace_vector_target\n"
      "mov r3, r0\n"
      "pop {r0, r2}\n"
      "str r3, [r0, #28]\n"
      "mov sp, r0\n"
      "msr apsr_nzcvq, r2\n"
      "ldmia.w sp!, {r0-r4, r12, lr}\n"
      "ldr.w pc, [sp], #4\n");
}

void h2_bk_pm_vector_install(void) {
  if (s_vectors_installed != 0u) return;
  uint32_t flags;
  __asm volatile("mrs %0, primask\ncpsid i" : "=r"(flags)::"memory");
  const uint32_t old_base = SCB->VTOR;
  const volatile uint32_t* source = (const volatile uint32_t*)old_base;
  for (unsigned index = 0u; index < H2_BK_PM_TRACE_VECTOR_COUNT; ++index) {
    const uint32_t original = source[index];
    s_original_vectors[index] = original;
    s_trace_vectors[index] = index >= 2u && original != 0u
                                 ? (uint32_t)(uintptr_t)h2_bk_trace_vector_entry
                                 : original;
  }
  s_vectors_installed = 1u;
  __DSB();
  SCB->VTOR = (uint32_t)(uintptr_t)s_trace_vectors;
  __DSB();
  __ISB();
  __asm volatile("msr primask, %0" ::"r"(flags) : "memory");
  BK_LOGW("h2-pm", "H2_CP_VECTOR old=%08lx new=%08lx active=%08lx count=%u\r\n",
          (unsigned long)old_base, (unsigned long)(uintptr_t)s_trace_vectors,
          (unsigned long)SCB->VTOR, (unsigned)H2_BK_PM_TRACE_VECTOR_COUNT);
}
