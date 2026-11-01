/* Temporary diagnosis of the NMI watchdog during H106 lifecycle transitions.
 * Capture before dump_prologue changes IRQ routing; emit to captured UART
 * afterwards. This diagnostic does not write or clear the Flash dump area. */
#include <stdint.h>
#include "bk_fault_observer.h"
#include "armstar.h"
#include "common/bk_assert.h"
#include "components/log.h"
#include "reset_reason.h"
#include "soc/reg_base.h"
#include "sys_ll.h"
#include "timer_ll.h"
#include "h2_bk_pm_observer.h"

typedef struct {
  uint32_t ready;
  uint32_t reason;
  uint32_t pc;
  uint32_t lr;
  uint32_t primask;
  uint32_t irq_enable[2];
  uint32_t irq_status[2];
  uint32_t nvic_enable[2];
  uint32_t nvic_pending[2];
  uint32_t peripheral_clocks;
  uint32_t clock_select;
  uint32_t timer_control;
  uint32_t timer_period;
  uint32_t timer_global;
  uint32_t systick_control;
  uint32_t sleep_control;
  uint32_t reboot_tag;
  uint32_t watchdog_control;
  uint32_t watchdog_global;
  uint32_t aon_watchdog_control;
} h2_bk_fault_snapshot_t;

static volatile h2_bk_fault_snapshot_t s_fault;
static volatile uint32_t s_capture_entered;
static volatile char s_enter_prefix[] = "\r\nH2_CP!\r\n";
static volatile char s_early_prefix[] = "\r\nH2_BK_CP_EARLY ";
static volatile char s_prepare_prefix[] = "\r\nH2_BK_CP_PREP ";

__attribute__((section(".iram"), noinline)) static int fault_put_char(
    uint32_t value) {
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
    if (!fault_put_char(digit < 10u ? '0' + digit : 'a' + digit - 10u))
      return 0;
  }
  return fault_put_char(' ');
}

/* Positional reason, PC, LR, NMI watchdog control, AON watchdog control.
 * Capture is observable even if later exception coordination cannot finish.
 * No FIFO wait, allocation, lock, formatter or Flash persistence is used. */
__attribute__((section(".iram"), noinline)) static void fault_emit_early(void) {
  for (unsigned i = 0u; i < sizeof(s_early_prefix) - 1u; ++i)
    if (!fault_put_char((uint8_t)s_early_prefix[i])) return;
  if (!fault_put_hex(s_fault.reason) || !fault_put_hex(s_fault.pc) ||
      !fault_put_hex(s_fault.lr) || !fault_put_hex(s_fault.watchdog_control) ||
      !fault_put_hex(s_fault.aon_watchdog_control))
    return;
  if (!fault_put_char('\r')) return;
  (void)fault_put_char('\n');
}

__attribute__((section(".iram"), noinline)) static void fault_emit_preparation(
    void) {
  for (unsigned i = 0u; i < sizeof(s_prepare_prefix) - 1u; ++i)
    if (!fault_put_char((uint8_t)s_prepare_prefix[i])) return;
  if (!fault_put_hex(h2_bk_pm_fault_observation.stage) ||
      !fault_put_hex(h2_bk_pm_fault_observation.uart_tx_gpio_before) ||
      !fault_put_hex(h2_bk_pm_fault_observation.uart_snapshot_valid))
    return;
  if (!fault_put_char('\r')) return;
  (void)fault_put_char('\n');
}

/* The SDK reaches capture at both NMI entry and the later dump handler.
 * Preserve the first sample, before its watchdog feed and IRQ masking. */
__attribute__((section(".iram"), noinline)) int bk_fault_context_enter(
    uint32_t reason, uint32_t exc_return, uint32_t stack) {
  (void)reason;
  (void)exc_return;
  (void)stack;
  if (s_capture_entered != 0u) return 0;
  s_capture_entered = 1u;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_FAULT_ENTRY, reason);
  h2_bk_pm_fault_uart_restore();
  __asm volatile("dmb sy" ::: "memory");
  for (unsigned i = 0u; i < sizeof(s_enter_prefix) - 1u; ++i)
    if (!fault_put_char((uint8_t)s_enter_prefix[i])) break;
  fault_emit_preparation();
  return 1;
}

__attribute__((section(".iram"), noinline)) void bk_fault_context_observe(
    uint32_t reason, const SAVED_CONTEXT* context) {
  timer_hw_t* timer = (timer_hw_t*)SOC_TIMER0_REG_BASE;
  s_fault.ready = 0u;
  s_fault.reason = reason;
  s_fault.pc = context->pc;
  s_fault.lr = context->lr;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_FAULT_PC, s_fault.pc);
  s_fault.primask = context->primask;
  s_fault.irq_enable[0] = sys_ll_get_cpu0_int_0_31_en_value();
  s_fault.irq_enable[1] = sys_ll_get_cpu0_int_32_63_en_value();
  s_fault.irq_status[0] = sys_ll_get_cpu0_int_0_31_status_value();
  s_fault.irq_status[1] = sys_ll_get_cpu0_int_32_63_status_value();
  s_fault.nvic_enable[0] = NVIC->ISER[0];
  s_fault.nvic_enable[1] = NVIC->ISER[1];
  s_fault.nvic_pending[0] = NVIC->ISPR[0];
  s_fault.nvic_pending[1] = NVIC->ISPR[1];
  s_fault.peripheral_clocks = sys_ll_get_cpu_device_clk_enable_value();
  s_fault.clock_select = sys_ll_get_cpu_clk_div_mode1_value();
  s_fault.timer_control = timer->group[0].ctrl.v;
  s_fault.timer_period = timer->group[0].timer_cnt[2];
  s_fault.timer_global = timer->group[0].global_ctrl.v;
  s_fault.systick_control = SysTick->CTRL;
  s_fault.sleep_control = SCB->SCR;
  s_fault.reboot_tag = *(volatile uint32_t*)REBOOT_TAG_ADDR;
  s_fault.watchdog_control = *(volatile uint32_t*)(SOC_WDT_REG_BASE + 4u * 4u);
  s_fault.watchdog_global = *(volatile uint32_t*)(SOC_WDT_REG_BASE + 4u * 2u);
  s_fault.aon_watchdog_control = *(volatile uint32_t*)SOC_AON_WDT_REG_BASE;
  __asm volatile("dmb sy" ::: "memory");
  s_fault.ready = 1u;
  fault_emit_early();
}

__attribute__((section(".iram"), noinline)) void bk_fault_context_persist(
    uint32_t reason, const SAVED_CONTEXT* context) {
  (void)context;
  BK_DUMP_OUT(
      "H2_BK_CP_EARLY_FAULT ready=%lu reason=%lx pc=%08lx lr=%08lx primask=%lx\r\n",
      (unsigned long)s_fault.ready, (unsigned long)reason,
      (unsigned long)s_fault.pc, (unsigned long)s_fault.lr,
      (unsigned long)s_fault.primask);
  BK_DUMP_OUT(
      "H2_BK_CP_IRQ "
      "irq_en0=%08lx irq_en1=%08lx irq_st0=%08lx irq_st1=%08lx\r\n",
      (unsigned long)s_fault.irq_enable[0],
      (unsigned long)s_fault.irq_enable[1],
      (unsigned long)s_fault.irq_status[0],
      (unsigned long)s_fault.irq_status[1]);
  BK_DUMP_OUT(
      "H2_BK_CP_NVIC "
      "nvic_en0=%08lx nvic_en1=%08lx nvic_pend0=%08lx nvic_pend1=%08lx\r\n",
      (unsigned long)s_fault.nvic_enable[0],
      (unsigned long)s_fault.nvic_enable[1],
      (unsigned long)s_fault.nvic_pending[0],
      (unsigned long)s_fault.nvic_pending[1]);
  BK_DUMP_OUT(
      "H2_BK_CP_CLOCK clocks=%08lx clksel=%08lx timer_ctrl=%08lx "
      "timer_period=%08lx\r\n",
      (unsigned long)s_fault.peripheral_clocks,
      (unsigned long)s_fault.clock_select, (unsigned long)s_fault.timer_control,
      (unsigned long)s_fault.timer_period);
  BK_DUMP_OUT("H2_BK_CP_SLEEP timer_global=%08lx systick=%08lx scr=%08lx\r\n",
              (unsigned long)s_fault.timer_global,
              (unsigned long)s_fault.systick_control,
              (unsigned long)s_fault.sleep_control);
  BK_DUMP_OUT(
      "H2_BK_CP_WDT reboot_tag=%08lx wdt_ctrl=%08lx wdt_global=%08lx\r\n",
      (unsigned long)s_fault.reboot_tag,
      (unsigned long)s_fault.watchdog_control,
      (unsigned long)s_fault.watchdog_global);
  BK_DUMP_OUT("H2_BK_CP_AON wdt_ctrl=%08lx\r\n",
              (unsigned long)s_fault.aon_watchdog_control);
  BK_DUMP_OUT("H2_BK_CP_PM stage=%lu gpio=%08lx saved=%08lx valid=%lu\r\n",
              (unsigned long)h2_bk_pm_fault_observation.stage,
              (unsigned long)h2_bk_pm_fault_observation.uart_tx_gpio_before,
              (unsigned long)h2_bk_pm_fault_observation.uart_tx_gpio_saved,
              (unsigned long)h2_bk_pm_fault_observation.uart_snapshot_valid);
  BK_DUMP_OUT("H2_BK_CP_PM_COUNTS lv=%lu feed=%lu\r\n",
              (unsigned long)h2_bk_pm_fault_observation.low_voltage_calls,
              (unsigned long)h2_bk_pm_fault_observation.aon_feeds);
  s_fault.ready = 0u;
}
