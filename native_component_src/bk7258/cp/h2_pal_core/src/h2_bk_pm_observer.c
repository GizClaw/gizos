/* Temporary UART-only observation of the unexplained H106 reset.
 * Link wrappers call the original SDK operation exactly once and preserve its
 * result. They neither change watchdog periods nor change the sleep decision.
 * All state and the reporting timer live for the diagnostic image lifetime. */
#include "h2_bk_pm_observer.h"

#include "components/log.h"
#include "driver/aon_wdt.h"
#include "driver/flash.h"
#include "driver/pwr_clk.h"
#include "driver/aon_rtc.h"
#include "FreeRTOS.h"
#include "task.h"
#include "arch_interrupt.h"
#include "gpio_ll.h"
#include "os/os.h"
#include "soc/reg_base.h"
#include "sys_ll.h"

extern void __real_sys_drv_enter_low_voltage(void);
extern bk_err_t __real_bk_aon_wdt_feed(void);
extern bk_err_t __real_bk_aon_wdt_stop(void);
extern bk_err_t __real_gpio_enter_low_power(void* param);
extern bk_err_t __real_gpio_exit_low_power(void* param);
extern bk_err_t __real_bk_flash_power_saving_enter(void);
extern bool __real_sys_hal_set_cp_sleep_vote_and_check_subcores_enter_wfi(void);
extern void __real_sys_drv_enter_normal_sleep(uint32_t peri_clk);
extern void __real_vPortSuppressTicksAndSleep(TickType_t expected);
extern void __real_vTaskStepTick(TickType_t ticks);
extern uint64_t __real_bk_aon_rtc_get_ms(void);
extern int_group_isr_t __real_arch_interrupt_get_handler(uint32_t number);
extern void __real_vPortEnterCritical(void);
extern void __real_vPortExitCritical(void);
extern void __real_vPortYield(void);
extern void __real_shell_log_flush(void);

/* CP has one core. The idle path writes these aligned SRAM words while IRQs
 * are masked; its timer task can only sample before or after that interval. */
static volatile uint32_t s_lv_enter_count;
static volatile uint32_t s_lv_exit_count;
static volatile uint32_t s_lv_phase;
static volatile uint32_t s_aon_feed_count;
static volatile uint32_t s_aon_stop_count;
static volatile uint32_t s_aon_last_feed_control;
static beken_timer_t s_report_timer;
static uint32_t s_sample_count;
static int s_started;
static uint32_t s_uart_tx_gpio_saved;
static uint32_t s_uart_snapshot_valid;
volatile h2_bk_pm_fault_observation_t h2_bk_pm_fault_observation;
static volatile uint32_t s_trace_enabled;
static volatile uint32_t s_trace_sequence;
static volatile uint32_t s_resume_pending;
static volatile uint32_t s_resume_depth;

static inline __attribute__((always_inline)) bool resume_thread_mode(void) {
  uint32_t exception;
  __asm volatile("mrs %0, ipsr" : "=r"(exception));
  return exception == 0u;
}

static inline __attribute__((always_inline)) uint32_t resume_context(void) {
  uint32_t flags;
  __asm volatile("mrs %0, primask" : "=r"(flags));
  const uint32_t owner = *(volatile uint32_t*)(PWR_MNG_FLASH_SHARED_ADDR + 8u);
  /* Keep invalid ownership distinguishable instead of truncating it to a CPU.
   */
  return ((owner <= 3u ? owner : 0xffu) << 8) | (flags & 1u);
}

/* Exact 6bbf SDK layout: its last declared PM field ends at byte84.
 * The two records occupy bytes88..127, before the Flash lock at byte128.
 * A new SDK layout must fail this diagnostic build and be audited again. */
#define H2_BK_PM_TRACE_OFFSET \
  (PWR_MNG_PM_RESERVED_SIZE - sizeof(h2_bk_pm_trace_slots_t))
#define H2_BK_PM_TRACE_SLOTS \
  PWR_MNG_PM_FIELD(h2_bk_pm_trace_slots_t, H2_BK_PM_TRACE_OFFSET)
_Static_assert(sizeof(FIXED_ADDR_PM_MODULE_LV_SLEEP_STATE) == 8u,
               "revalidate retained trace for the new SDK PM layout");
_Static_assert(H2_BK_PM_TRACE_OFFSET >= 84u,
               "retained trace overlaps declared SDK PM fields");
_Static_assert(PWR_MNG_FLASH_SHARED_OFFSET == PWR_MNG_PM_RESERVED_SIZE,
               "revalidate shared Flash boundary");

__attribute__((section(".iram"), noinline)) void h2_bk_pm_trace_event(
    uint32_t event, uint32_t value) {
  if (s_trace_enabled == 0u || !h2_bk_pm_trace_event_valid(event)) return;
  /* Normal IRQ entries now publish as well. Keep a complete slot publication
   * indivisible by ordinary IRQs, then restore the exact incoming mask.
   * A fatal NMI may interrupt this bounded write; the other slot survives. */
  uint32_t flags;
  __asm volatile("mrs %0, primask\ncpsid i" : "=r"(flags)::"memory");
  const uint32_t sequence = s_trace_sequence + 1u;
  s_trace_sequence = sequence;
  const uint32_t meta = (event << 16) | (s_lv_phase & 0xffffu);
  h2_bk_pm_trace_publish(&H2_BK_PM_TRACE_SLOTS[sequence & 1u], sequence, meta,
                         value);
  __asm volatile("msr primask, %0" ::"r"(flags) : "memory");
}

static void start_retained_trace(void) {
  bool have_previous = false;
  uint32_t latest = 0u;
  for (unsigned slot = 0u; slot < 2u; ++slot) {
    h2_bk_pm_trace_record_t previous = {0};
    if (!h2_bk_pm_trace_read(&H2_BK_PM_TRACE_SLOTS[slot], &previous)) {
      BK_LOGW("h2-pm", "H2_CP_TRACE slot=%u valid=0\r\n", slot);
      continue;
    }
    BK_LOGW("h2-pm",
            "H2_CP_TRACE slot=%u valid=1 seq=%lu ev=%lu pm=%lu v=%08lx\r\n",
            slot, (unsigned long)previous.sequence,
            (unsigned long)(previous.meta >> 16),
            (unsigned long)(previous.meta & 0xffffu),
            (unsigned long)previous.value);
    if (!have_previous || h2_bk_pm_trace_newer(previous.sequence, latest)) {
      latest = previous.sequence;
      have_previous = true;
    }
  }
  s_trace_sequence = have_previous ? latest : 0u;
  s_trace_enabled = 1u;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_BOOT, FIXED_ADDR_CP_RESET_REASON);
}

/* Stages bracket the actual SDK calls without logging, allocating, taking a
 * lock, or waiting in the IRQ-masked PM path:
 * 1/2 AON feed, 3/4 GPIO prepare, 5/6 Flash prepare, 7/8 low-voltage HAL,
 * 9 GPIO restore, 0 after GPIO restore. HAL may return without entering WFI. */

__attribute__((section(".iram"), noinline)) void
__wrap_sys_drv_enter_low_voltage(void) {
  ++s_lv_enter_count;
  s_lv_phase = 7u;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_TRANSITION,
                       *(volatile uint32_t*)(SOC_AON_PMU_REG_BASE + 12u));
  __real_sys_drv_enter_low_voltage();
  ++s_lv_exit_count;
  s_lv_phase = 8u;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_TRANSITION,
                       *(volatile uint32_t*)(SOC_AON_PMU_REG_BASE + 12u));
}

__attribute__((section(".iram"), noinline)) bk_err_t
__wrap_bk_aon_wdt_feed(void) {
  s_lv_phase = 1u;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_TRANSITION,
                       *(volatile uint32_t*)SOC_AON_WDT_REG_BASE);
  const bk_err_t result = __real_bk_aon_wdt_feed();
  ++s_aon_feed_count;
  s_aon_last_feed_control = *(volatile uint32_t*)SOC_AON_WDT_REG_BASE;
  s_lv_phase = 2u;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_TRANSITION, s_aon_last_feed_control);
  return result;
}

__attribute__((section(".iram"), noinline)) bk_err_t
__wrap_bk_aon_wdt_stop(void) {
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_AON_STOP_ENTER,
                       *(volatile uint32_t*)SOC_AON_WDT_REG_BASE);
  const bk_err_t result = __real_bk_aon_wdt_stop();
  ++s_aon_stop_count;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_AON_STOP_EXIT,
                       *(volatile uint32_t*)SOC_AON_WDT_REG_BASE);
  return result;
}

__attribute__((section(".iram"), noinline)) bk_err_t
__wrap_gpio_enter_low_power(void* param) {
  s_lv_phase = 3u;
  h2_bk_pm_trace_event(
      H2_BK_PM_TRACE_TRANSITION,
      gpio_ll_get_value((gpio_hw_t*)GPIO_LL_REG_BASE, UART0_TX_PIN));
  const bk_err_t result = __real_gpio_enter_low_power(param);
  s_lv_phase = 4u;
  h2_bk_pm_trace_event(
      H2_BK_PM_TRACE_TRANSITION,
      gpio_ll_get_value((gpio_hw_t*)GPIO_LL_REG_BASE, UART0_TX_PIN));
  return result;
}

__attribute__((section(".iram"), noinline)) bk_err_t
__wrap_gpio_exit_low_power(void* param) {
  s_lv_phase = 9u;
  h2_bk_pm_trace_event(
      H2_BK_PM_TRACE_TRANSITION,
      gpio_ll_get_value((gpio_hw_t*)GPIO_LL_REG_BASE, UART0_TX_PIN));
  const bk_err_t result = __real_gpio_exit_low_power(param);
  s_lv_phase = 0u;
  h2_bk_pm_trace_event(
      H2_BK_PM_TRACE_TRANSITION,
      gpio_ll_get_value((gpio_hw_t*)GPIO_LL_REG_BASE, UART0_TX_PIN));
  return result;
}

__attribute__((section(".iram"), noinline)) bk_err_t
__wrap_bk_flash_power_saving_enter(void) {
  s_lv_phase = 5u;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_TRANSITION,
                       *(volatile uint32_t*)(PWR_MNG_FLASH_SHARED_ADDR + 8u));
  const bk_err_t result = __real_bk_flash_power_saving_enter();
  s_lv_phase = 6u;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_TRANSITION,
                       *(volatile uint32_t*)(PWR_MNG_FLASH_SHARED_ADDR + 8u));
  return result;
}

__attribute__((section(".iram"), noinline)) bool
__wrap_sys_hal_set_cp_sleep_vote_and_check_subcores_enter_wfi(void) {
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_HANDSHAKE_ENTER,
                       *(volatile uint32_t*)(SOC_AON_PMU_REG_BASE + 12u));
  const bool result =
      __real_sys_hal_set_cp_sleep_vote_and_check_subcores_enter_wfi();
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_HANDSHAKE_EXIT,
                       *(volatile uint32_t*)(SOC_AON_PMU_REG_BASE + 12u));
  return result;
}

__attribute__((section(".iram"), noinline)) void
__wrap_sys_drv_enter_normal_sleep(uint32_t peri_clk) {
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_NORMAL_ENTER,
                       sys_ll_get_cpu_device_clk_enable_value());
  __real_sys_drv_enter_normal_sleep(peri_clk);
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_NORMAL_EXIT,
                       sys_ll_get_cpu0_int_0_31_en_value());
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_SLEEP_RETURN_FLASH_OWNER,
                       *(volatile uint32_t*)(PWR_MNG_FLASH_SHARED_ADDR + 8u));
}

__attribute__((section(".iram"), noinline)) void
__wrap_vPortSuppressTicksAndSleep(TickType_t expected) {
  s_resume_pending = 0u;
  s_resume_depth = 0u;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_TICKLESS_ENTER, (uint32_t)expected);
  __real_vPortSuppressTicksAndSleep(expected);
  uint32_t flags;
  __asm volatile("mrs %0, primask" : "=r"(flags));
  s_resume_pending = 1u;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_TICKLESS_EXIT, flags);
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_TICKLESS_RETURN_CONTEXT,
                       resume_context());
}

/* Observe the first thread-mode critical section after tickless return. The
 * actual return PC identifies its caller; the interval alone does not imply
 * xTaskResumeAll. IRQ handlers do not consume this diagnostic interval.
 * These counters never participate in the SDK's real nesting or scheduling. */
__attribute__((section(".iram"), noinline)) void __wrap_vPortEnterCritical(
    void) {
  const uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
  const bool observe =
      resume_thread_mode() && (s_resume_pending != 0u || s_resume_depth != 0u);
  if (observe) {
    s_resume_pending = 0u;
    ++s_resume_depth;
    h2_bk_pm_trace_event(H2_BK_PM_TRACE_CRITICAL_ENTER, caller);
    h2_bk_pm_trace_event(H2_BK_PM_TRACE_CRITICAL_ENTER_CONTEXT,
                         resume_context());
  }
  __real_vPortEnterCritical();
  if (observe) h2_bk_pm_trace_event(H2_BK_PM_TRACE_CRITICAL_ENTERED, caller);
}

__attribute__((section(".iram"), noinline)) void __wrap_vPortExitCritical(
    void) {
  const uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
  const bool observe = resume_thread_mode() && s_resume_depth != 0u;
  if (observe) {
    h2_bk_pm_trace_event(H2_BK_PM_TRACE_CRITICAL_EXIT, caller);
    /* Close the outer interval before the real exit can enable IRQs/yield. */
    --s_resume_depth;
  }
  __real_vPortExitCritical();
  if (observe)
    h2_bk_pm_trace_event(H2_BK_PM_TRACE_CRITICAL_EXITED_CONTEXT,
                         resume_context());
}

__attribute__((section(".iram"), noinline)) void __wrap_vPortYield(void) {
  const uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
  const bool observe =
      resume_thread_mode() && (s_resume_pending != 0u || s_resume_depth != 0u);
  if (observe) h2_bk_pm_trace_event(H2_BK_PM_TRACE_YIELD_ENTER, caller);
  __real_vPortYield();
  if (observe) h2_bk_pm_trace_event(H2_BK_PM_TRACE_YIELD_EXIT, caller);
}

__attribute__((section(".iram"), noinline)) void __wrap_shell_log_flush(void) {
  const uint32_t caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
  const bool observe =
      resume_thread_mode() && (s_resume_pending != 0u || s_resume_depth != 0u);
  if (observe) h2_bk_pm_trace_event(H2_BK_PM_TRACE_FLUSH_ENTER, caller);
  __real_shell_log_flush();
  if (observe) h2_bk_pm_trace_event(H2_BK_PM_TRACE_FLUSH_EXIT, caller);
}

__attribute__((section(".iram"), noinline)) void __wrap_vTaskStepTick(
    TickType_t ticks) {
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_STEP_ENTER, (uint32_t)ticks);
  __real_vTaskStepTick(ticks);
  uint32_t flags;
  __asm volatile("mrs %0, primask" : "=r"(flags));
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_STEP_EXIT, flags);
}

__attribute__((section(".iram"), noinline)) uint64_t
__wrap_bk_aon_rtc_get_ms(void) {
  uint32_t flags;
  __asm volatile("mrs %0, primask" : "=r"(flags));
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_RTC_ENTER, flags);
  const uint64_t result = __real_bk_aon_rtc_get_ms();
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_RTC_EXIT, (uint32_t)result);
  return result;
}

__attribute__((section(".iram"), noinline)) int_group_isr_t
__wrap_arch_interrupt_get_handler(uint32_t number) {
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_IRQ_LOOKUP, number);
  const int_group_isr_t result = __real_arch_interrupt_get_handler(number);
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_IRQ_CALLBACK,
                       (uint32_t)(uintptr_t)result);
  return result;
}

/* GPIO preparation precedes Flash preparation in the SDK PM path and can
 * remove UART0's TX mux before the watchdog fires. At fault entry only, save
 * that original state and republish the known working TX configuration.
 * This one MMIO write leaves normal execution, the rail GPIO, UART clocks,
 * watchdogs, and the original PM decision untouched. */
__attribute__((section(".iram"), noinline)) void h2_bk_pm_fault_uart_restore(
    void) {
  gpio_hw_t* gpio = (gpio_hw_t*)GPIO_LL_REG_BASE;
  h2_bk_pm_fault_observation.stage = s_lv_phase;
  h2_bk_pm_fault_observation.uart_tx_gpio_before =
      gpio_ll_get_value(gpio, UART0_TX_PIN);
  h2_bk_pm_fault_observation.uart_tx_gpio_saved = s_uart_tx_gpio_saved;
  h2_bk_pm_fault_observation.uart_snapshot_valid = s_uart_snapshot_valid;
  h2_bk_pm_fault_observation.low_voltage_calls = s_lv_enter_count;
  h2_bk_pm_fault_observation.aon_feeds = s_aon_feed_count;
  if (s_uart_snapshot_valid != 0u)
    gpio_ll_set_value(gpio, UART0_TX_PIN, s_uart_tx_gpio_saved);
  __asm volatile("dmb sy" ::: "memory");
}

static void report_pm_state(void* unused) {
  (void)unused;
  h2_bk_pm_trace_event(H2_BK_PM_TRACE_SAMPLE, rtos_get_time());
  ++s_sample_count;
  /* Keep each line below the SDK formatter's truncation boundary. Counts of
   * low-voltage calls include HAL early returns; they do not prove WFI entry.
   */
  BK_LOGW("h2-pm", "H2_CP_PM n=%lu ms=%lu enter=%lu exit=%lu phase=%lu\r\n",
          (unsigned long)s_sample_count, (unsigned long)rtos_get_time(),
          (unsigned long)s_lv_enter_count, (unsigned long)s_lv_exit_count,
          (unsigned long)s_lv_phase);
  BK_LOGW(
      "h2-pm", "H2_CP_PM_AON n=%lu feed=%lu stop=%lu last=%08lx now=%08lx\r\n",
      (unsigned long)s_sample_count, (unsigned long)s_aon_feed_count,
      (unsigned long)s_aon_stop_count, (unsigned long)s_aon_last_feed_control,
      (unsigned long)*(volatile uint32_t*)SOC_AON_WDT_REG_BASE);
  BK_LOGW("h2-pm", "H2_CP_PM_HW n=%lu nmi=%08lx r3=%08lx clocks=%08lx\r\n",
          (unsigned long)s_sample_count,
          (unsigned long)*(volatile uint32_t*)(SOC_WDT_REG_BASE + 16u),
          (unsigned long)*(volatile uint32_t*)(SOC_AON_PMU_REG_BASE + 12u),
          (unsigned long)sys_ll_get_cpu_device_clk_enable_value());
  BK_LOGW("h2-pm", "H2_CP_PM_GPIO n=%lu saved=%08lx valid=%lu\r\n",
          (unsigned long)s_sample_count, (unsigned long)s_uart_tx_gpio_saved,
          (unsigned long)s_uart_snapshot_valid);
}

void h2_bk_pm_diagnostics_start(void) {
  if (s_started) return;
  gpio_hw_t* gpio = (gpio_hw_t*)GPIO_LL_REG_BASE;
  __typeof__(gpio->gpio_num[UART0_TX_PIN].cfg) config;
  config.v = gpio_ll_get_value(gpio, UART0_TX_PIN);
  s_uart_tx_gpio_saved = config.v;
  s_uart_snapshot_valid = config.gpio_2_func_en != 0u;
  h2_bk_pm_vector_install();
  start_retained_trace();
  bk_err_t result =
      rtos_init_timer(&s_report_timer, 1000u, report_pm_state, NULL);
  if (result != BK_OK) {
    BK_LOGW("h2-pm", "H2_CP_PM_DIAG ready=0 stage=timer_init rc=%d\r\n",
            result);
    return;
  }
  result = rtos_start_timer(&s_report_timer);
  if (result != BK_OK) {
    const bk_err_t cleanup = rtos_deinit_timer(&s_report_timer);
    BK_LOGW("h2-pm",
            "H2_CP_PM_DIAG ready=0 stage=timer_start rc=%d cleanup=%d\r\n",
            result, cleanup);
    return;
  }
  s_started = 1;
  BK_LOGW("h2-pm", "H2_CP_PM_DIAG ready=1 period_ms=1000\r\n");
  report_pm_state(NULL);
}
