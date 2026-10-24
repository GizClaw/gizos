#include "sdkconfig.h"

#if CONFIG_H2_BK_PLATFORM_FAULT_TRACE && CONFIG_SOC_SMP && CONFIG_SHELL_ASYNCLOG

#include "h2_bk_fault_context.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef H2_BK_FAULT_HOST_TEST
#include "h2_bk_fault_test_sdk.h"
#else
#include "arch_interrupt.h"
#include "armstar.h"
#include <os/os.h>
#define H2_BK_FAULT_CALLSITE() ((uintptr_t)__builtin_return_address(0))
#endif

typedef struct {
  char data[512];
  size_t length;
  bool overflow;
} fault_line_t;

/* Each writer runs with its local interrupts masked. A recursive fault must
 * not recurse into the UART; no cross-core lock belongs in this path. */
static volatile bool s_recorded[CONFIG_CPU_CNT];
static volatile bool s_entered[CONFIG_CPU_CNT];
static volatile bool s_flushing[CONFIG_CPU_CNT];
/* Fatal handlers use MSP, and power-management flushes can run on the small
 * idle stack. Keep scratch in SRAM, separate for each core and nested phase. */
static fault_line_t s_context_lines[CONFIG_CPU_CNT];
static fault_line_t s_entry_lines[CONFIG_CPU_CNT];
static fault_line_t s_flush_lines[CONFIG_CPU_CNT];

static fault_line_t* begin_line(fault_line_t* line) {
  line->length = 0u;
  line->overflow = false;
  return line;
}

static void append(fault_line_t* line, const char* value) {
  while (*value != '\0') {
    if (line->length == sizeof(line->data)) {
      line->overflow = true;
      return;
    }
    line->data[line->length++] = *value++;
  }
}

static void field(fault_line_t* line, const char* name, uint32_t value) {
  static const char digits[] = "0123456789abcdef";
  append(line, " ");
  append(line, name);
  append(line, "=0x");
  for (unsigned shift = 32u; shift != 0u; shift -= 4u) {
    const char digit[] = {digits[(value >> (shift - 4u)) & 15u], '\0'};
    append(line, digit);
  }
}

static void emit(fault_line_t* line) {
  append(line, "\r\n");
  if (!line->overflow)
    (void)emergency_uart_write_buf(CONFIG_DUMP_UART_PRINT_PORT, line->data,
                                   (uint16_t)line->length);
}

void h2_bk_fault_context_enter(uint32_t reason, uint32_t exc_return,
                               uint32_t stack) {
  /* SDK rtos_get_core_id adds the physical CPU offset; task policies and
   * these per-AP-core slots use the kernel's zero-based ordinal. */
  const uint32_t core = (uint32_t)portGET_CORE_ID();
  if (core >= CONFIG_CPU_CNT || s_entered[core]) return;
  s_entered[core] = true;
  fault_line_t* line = begin_line(&s_entry_lines[core]);
  append(line, "\r\nH2_BK_NATIVE_FAULT_ENTER");
  field(line, "core", core);
  field(line, "reason", reason);
  field(line, "exc_return", exc_return);
  field(line, "stack", stack);
  field(line, "cfsr", SCB->CFSR);
  field(line, "hfsr", SCB->HFSR);
  field(line, "bfar", SCB->BFAR);
  field(line, "mmfar", SCB->MMFAR);
  emit(line);
}

void h2_bk_fault_context_observe(uint32_t reason,
                                 const SAVED_CONTEXT* context) {
  const uint32_t core = (uint32_t)portGET_CORE_ID();
  if (context == NULL || core >= CONFIG_CPU_CNT || s_recorded[core]) return;
  s_recorded[core] = true;
  fault_line_t* line = begin_line(&s_context_lines[core]);
  append(line, "\r\nH2_BK_NATIVE_FAULT");
  field(line, "core", core);
  field(line, "reason", reason);
  field(line, "pc", (uint32_t)context->pc);
  field(line, "lr", (uint32_t)context->lr);
  field(line, "sp", (uint32_t)context->sp);
  field(line, "r0", (uint32_t)context->r0);
  field(line, "r1", (uint32_t)context->r1);
  field(line, "r2", (uint32_t)context->r2);
  field(line, "r3", (uint32_t)context->r3);
  field(line, "r12", (uint32_t)context->r12);
  field(line, "xpsr", (uint32_t)context->xpsr);
  field(line, "exc_return", (uint32_t)context->control);
  field(line, "primask", (uint32_t)context->primask);
  field(line, "cfsr", SCB->CFSR);
  field(line, "hfsr", SCB->HFSR);
  field(line, "bfar", SCB->BFAR);
  field(line, "mmfar", SCB->MMFAR);
  emit(line);
}

void __real_shell_log_flush(void);

void __wrap_shell_log_flush(void) {
  const uint32_t core = (uint32_t)portGET_CORE_ID();
  if (__get_PRIMASK() != 0u && core < CONFIG_CPU_CNT && !s_flushing[core]) {
    s_flushing[core] = true;
    fault_line_t* line = begin_line(&s_flush_lines[core]);
    append(line, "\r\nH2_BK_LOG_FLUSH_MASKED");
    field(line, "core", core);
    field(line, "caller", (uint32_t)H2_BK_FAULT_CALLSITE());
    field(line, "cfsr", SCB->CFSR);
    emit(line);
    s_flushing[core] = false;
  }
  __real_shell_log_flush();
}

#endif
