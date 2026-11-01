/* Temporary fault diagnosis. Early capture does not allocate, take a lock,
 * wait for UART space, or touch Flash. The full saved record is emitted only
 * from the SDK's later persist hook; nothing is persisted to Flash. */
#include <stdint.h>
#include "bk_fault_observer.h"
#include "armstar.h"
#include "common/bk_assert.h"
#include "cpu_id.h"
#include "soc/reg_base.h"

typedef struct {
  uint32_t entered;
  uint32_t ready;
  uint32_t core;
  uint32_t reason;
  uint32_t pc;
  uint32_t lr;
  uint32_t sp;
  uint32_t msp;
  uint32_t psp;
  uint32_t primask;
  uint32_t basepri;
  uint32_t faultmask;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t mmfar;
  uint32_t bfar;
  uint32_t early_uart_dropped;
} h2_bk_ap_fault_t;

static volatile h2_bk_ap_fault_t s_ap_fault[3];
/* Keep the prefix in SRAM: a fault may have interrupted a Flash operation. */
static volatile char s_early_prefix[] = "\r\nH2_BK_AP_EARLY ";
static volatile char s_enter_prefix[] = "\r\nH2_AP!\r\n";

__attribute__((section(".iram"), noinline)) static uint32_t fault_core(void) {
  const uint32_t core = *(volatile uint32_t*)CPU_ID_ADDR;
  return core < 3u ? core : 0u;
}

__attribute__((section(".iram"), noinline)) static int fault_put_char(
    uint32_t value) {
  /* UART0 is the existing CP/dump UART, captured alongside AP UART1. These
   * offsets and TX-full bit match the pinned SDK emergency UART writer. */
  volatile uint32_t* status = (volatile uint32_t*)(SOC_UART0_REG_BASE + 0x18u);
  volatile uint32_t* fifo = (volatile uint32_t*)(SOC_UART0_REG_BASE + 0x1cu);
  if ((*status & (1u << 16)) != 0u) return 0;
  *fifo = value;
  return 1;
}

__attribute__((section(".iram"), noinline)) static int fault_put_hex(
    uint32_t value) {
  for (int shift = 28; shift >= 0; shift -= 4) {
    const uint32_t digit = (value >> shift) & 15u;
    const uint32_t character = digit < 10u ? '0' + digit : 'a' + digit - 10u;
    if (!fault_put_char(character)) return 0;
  }
  return fault_put_char(' ');
}

/* Positional hex fields: core, reason, interrupted PC, LR, CFSR. At most
 * 64 bytes, with one FIFO availability check per byte and no retry. Emit
 * scalars directly so this emergency path needs no stack buffer or formatter.
 */
__attribute__((section(".iram"), noinline)) static void fault_emit_early(
    volatile h2_bk_ap_fault_t* saved) {
  for (unsigned i = 0u; i < sizeof(s_early_prefix) - 1u; ++i) {
    if (!fault_put_char((uint8_t)s_early_prefix[i])) {
      saved->early_uart_dropped = 1u;
      return;
    }
  }
  if (!fault_put_hex(saved->core) || !fault_put_hex(saved->reason) ||
      !fault_put_hex(saved->pc) || !fault_put_hex(saved->lr) ||
      !fault_put_hex(saved->cfsr) || !fault_put_char('\r') ||
      !fault_put_char('\n'))
    saved->early_uart_dropped = 1u;
}

__attribute__((section(".iram"), noinline)) int bk_fault_context_enter(
    uint32_t reason, uint32_t exc_return, uint32_t stack) {
  (void)reason;
  (void)exc_return;
  (void)stack;
  volatile h2_bk_ap_fault_t* saved = &s_ap_fault[fault_core()];
  if (saved->entered != 0u) return 0;
  saved->entered = 1u;
  __asm volatile("dmb sy" ::: "memory");
  /* Preserve evidence that a fault entered even if inspecting the interrupted
   * stack faults again. This prefix also never waits for UART capacity. */
  for (unsigned i = 0u; i < sizeof(s_enter_prefix) - 1u; ++i) {
    if (!fault_put_char((uint8_t)s_enter_prefix[i])) {
      saved->early_uart_dropped = 1u;
      break;
    }
  }
  return 1;
}

__attribute__((section(".iram"), noinline)) void bk_fault_context_observe(
    uint32_t reason, const SAVED_CONTEXT* context) {
  const uint32_t core = fault_core();
  volatile h2_bk_ap_fault_t* saved = &s_ap_fault[core];
  if (context == 0 || saved->ready != 0u) return;
  saved->core = core;
  saved->reason = reason;
  saved->pc = context->pc;
  saved->lr = context->lr;
  saved->sp = context->sp;
  saved->msp = context->msp;
  saved->psp = context->psp;
  saved->primask = context->primask;
  saved->basepri = context->basepri;
  saved->faultmask = context->faultmask;
  saved->cfsr = SCB->CFSR;
  saved->hfsr = SCB->HFSR;
  saved->mmfar = SCB->MMFAR;
  saved->bfar = SCB->BFAR;
  __asm volatile("dmb sy" ::: "memory");
  saved->ready = 1u;
  fault_emit_early(saved);
}

__attribute__((section(".iram"), noinline)) void bk_fault_context_persist(
    uint32_t reason, const SAVED_CONTEXT* context) {
  (void)reason;
  (void)context;
  volatile h2_bk_ap_fault_t* saved = &s_ap_fault[fault_core()];
  BK_DUMP_OUT(
      "H2_BK_AP_FAULT core=%lu ready=%lu reason=%lx pc=%08lx lr=%08lx\r\n",
      (unsigned long)saved->core, (unsigned long)saved->ready,
      (unsigned long)saved->reason, (unsigned long)saved->pc,
      (unsigned long)saved->lr);
  BK_DUMP_OUT(
      "H2_BK_AP_STATUS cfsr=%08lx hfsr=%08lx mmfar=%08lx bfar=%08lx\r\n",
      (unsigned long)saved->cfsr, (unsigned long)saved->hfsr,
      (unsigned long)saved->mmfar, (unsigned long)saved->bfar);
  BK_DUMP_OUT("H2_BK_AP_STACK sp=%08lx msp=%08lx psp=%08lx\r\n",
              (unsigned long)saved->sp, (unsigned long)saved->msp,
              (unsigned long)saved->psp);
  BK_DUMP_OUT(
      "H2_BK_AP_MASK primask=%lx basepri=%lx faultmask=%lx dropped=%lu\r\n",
      (unsigned long)saved->primask, (unsigned long)saved->basepri,
      (unsigned long)saved->faultmask,
      (unsigned long)saved->early_uart_dropped);
}
