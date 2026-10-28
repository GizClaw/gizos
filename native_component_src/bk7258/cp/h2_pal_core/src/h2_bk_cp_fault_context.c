#include "sdkconfig.h"

#if CONFIG_H2_BK_PLATFORM_CP_FAULT_TRACE

#include "h2_bk_cp_fault_context.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef H2_BK_CP_FAULT_HOST_TEST
#include "h2_bk_cp_fault_test_sdk.h"
#define H2_CP_IRAM
#else
#include "armstar.h"
#include "bk_fault_observer.h"
#include <soc/reg_base.h>
#define H2_CP_IRAM __attribute__((section(".iram"), noinline))
#endif

/* CP has one core. Mark entry before touching the interrupted stack; a
 * secondary exception must not recursively format or inspect that stack.
 * Scratch and labels stay in SRAM so the early path needs no XIP literals.
 */
static volatile bool s_entered;
static uint32_t s_entry_stack;
static char s_line[512];
static volatile char s_labels[][32] = {
    "\r\nH2CP_ENTER=",
    "\r\nH2_BK_CP_FAULT",
    " reason=",
    " exc_return=",
    " stack=",
    " cfsr=",
    " hfsr=",
    " bfar=",
    " mmfar=",
    " pc=",
    " lr=",
    " sp=",
    " xpsr=",
    " r0=",
    " r1=",
    " r2=",
    " r3=",
    " r12=",
    " primask=",
    "\r\nH2_BK_CP_FAULT_REGS",
};

static H2_CP_IRAM size_t text(size_t offset, const volatile char* value) {
  while (*value != '\0' && offset < sizeof(s_line)) s_line[offset++] = *value++;
  return offset;
}

static H2_CP_IRAM size_t hexword(size_t offset, uint32_t value) {
  if (offset + 8u > sizeof(s_line)) return sizeof(s_line);
  for (unsigned shift = 32u; shift != 0u; shift -= 4u) {
    const unsigned digit = (value >> (shift - 4u)) & 15u;
    s_line[offset++] = (char)(digit < 10u ? '0' + digit : 'a' + digit - 10u);
  }
  return offset;
}

static H2_CP_IRAM size_t field(size_t offset, unsigned label, uint32_t value) {
  offset = text(offset, s_labels[label]);
  if (offset + 2u > sizeof(s_line)) return sizeof(s_line);
  s_line[offset++] = '0';
  s_line[offset++] = 'x';
  return hexword(offset, value);
}

#ifndef H2_BK_CP_FAULT_HOST_TEST
#if CONFIG_DUMP_UART_PRINT_PORT == 0
#define H2_CP_UART_BASE SOC_UART0_REG_BASE
#elif CONFIG_DUMP_UART_PRINT_PORT == 1
#define H2_CP_UART_BASE SOC_UART1_REG_BASE
#elif CONFIG_DUMP_UART_PRINT_PORT == 2
#define H2_CP_UART_BASE SOC_UART2_REG_BASE
#else
#error Unsupported CP diagnostic UART
#endif

static H2_CP_IRAM bool uart_full(void) {
  return (*(volatile uint32_t*)(H2_CP_UART_BASE + 0x18u) & (1u << 16u)) != 0u;
}

static H2_CP_IRAM void uart_putc(char value) {
  *(volatile uint32_t*)(H2_CP_UART_BASE + 0x1cu) = (uint8_t)value;
}
#else
#define uart_full h2_bk_cp_test_uart_full
#define uart_putc h2_bk_cp_test_uart_putc
#endif

static H2_CP_IRAM void emit(size_t size) {
  if (size + 2u > sizeof(s_line)) return;
  s_line[size++] = '\r';
  s_line[size++] = '\n';
  /* SDK emergency output first checks AP dump ownership. This earlier
   * record must precede that check too. The pinned SDK FIFO layout is
   * used directly; a total poll budget bounds a stopped UART's effect.
   */
  unsigned polls = 1000000u;
  for (size_t i = 0u; i < size; ++i) {
    while (uart_full()) {
      if (polls == 0u) return;
      --polls;
    }
    uart_putc(s_line[i]);
  }
}

H2_CP_IRAM int h2_bk_cp_fault_context_enter(uint32_t reason,
                                            uint32_t exc_return,
                                            uint32_t stack) {
  if (s_entered) return 0;
  s_entered = true;
  s_entry_stack = stack;
  (void)exc_return;
  /* A hardware reset can follow NMI before a full UART line drains.
   * Preserve an entry marker before inspecting the interrupted stack,
   * then spend the first short context record on PC/LR.
   */
  size_t size = text(0u, s_labels[0]);
  size = hexword(size, reason);
  emit(size);
  return 1;
}

H2_CP_IRAM void h2_bk_cp_fault_context_observe(uint32_t reason,
                                               const SAVED_CONTEXT* context) {
  if (context == NULL) return;
  size_t size = text(0u, s_labels[1]);
  size = field(size, 9u, (uint32_t)context->pc);
  size = field(size, 10u, (uint32_t)context->lr);
  emit(size);
  size = text(0u, s_labels[19]);
  size = field(size, 2u, reason);
  size = field(size, 11u, (uint32_t)context->sp);
  size = field(size, 12u, (uint32_t)context->xpsr);
  size = field(size, 3u, (uint32_t)context->control);
  size = field(size, 4u, s_entry_stack);
  size = field(size, 5u, SCB->CFSR);
  size = field(size, 6u, SCB->HFSR);
  size = field(size, 7u, SCB->BFAR);
  size = field(size, 8u, SCB->MMFAR);
  size = field(size, 13u, (uint32_t)context->r0);
  size = field(size, 14u, (uint32_t)context->r1);
  size = field(size, 15u, (uint32_t)context->r2);
  size = field(size, 16u, (uint32_t)context->r3);
  size = field(size, 17u, (uint32_t)context->r12);
  size = field(size, 18u, (uint32_t)context->primask);
  emit(size);
}

H2_CP_IRAM int bk_fault_context_enter(uint32_t reason, uint32_t exc_return,
                                      uint32_t stack) {
  return h2_bk_cp_fault_context_enter(reason, exc_return, stack);
}
H2_CP_IRAM void bk_fault_context_observe(uint32_t reason,
                                         const SAVED_CONTEXT* context) {
  h2_bk_cp_fault_context_observe(reason, context);
}

#endif
