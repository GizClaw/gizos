/* The actual SDK CP entries and frame parser are inserted below. Only MMIO,
 * Cortex-M address access, and the non-returning reset boundary are shims.
 */
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "h2_bk_cp_fault_context.h"
#include "h2_bk_cp_fault_test_sdk.h"

#define CONFIG_DEBUG_VERSION 0
#define CONFIG_DUMP_ENABLE H2_TEST_DUMP
#define CONFIG_AON_PMU_REG0_REFACTOR_DEV 0
#define CONFIG_CM_BACKTRACE 0
#define RESET_SOURCE_NMI_WDT 16u
#define REBOOT_TAG_REQ 0x1234u
#define BK_DUMP_OUT(...) ((void)0)

extern void h2_bk_cp_fault_capture(uint32_t reason, uint32_t lr, uint32_t sp)
    __attribute__((weak));
extern int h2_bk_cp_fault_context_enter(uint32_t reason, uint32_t exc_return,
                                        uint32_t stack) __attribute__((weak));
extern void h2_bk_cp_fault_context_observe(uint32_t reason,
                                           const SAVED_CONTEXT* context)
    __attribute__((weak));
static void user_except_handler_ex(uint32_t reason, uint32_t lr, uint32_t sp);

static jmp_buf escape;
static const char* mode;
static unsigned fpscr_reads, normal_dumps, raw_polls, context_reads;
static unsigned watchdog_feeds, masked;
static bool nested;
static char output[1600];
static size_t output_size;
static uint32_t main_stack[40], process_stack[40];
static uint32_t exc_return, alignment;
static const uint32_t main_address = 0x28001020u;
static const uint32_t process_address = 0x60001000u;
h2_cp_test_scb_t test_scb = {0x82u, 0x40000000u, 0x28012345u, 0x60012345u};
volatile const uint8_t build_version[] = "test";

bool h2_bk_cp_test_uart_full(void) {
  ++raw_polls;
  return strcmp(mode, "uart-full") == 0;
}

void h2_bk_cp_test_uart_putc(char value) {
  assert(output_size + 1u < sizeof(output));
  output[output_size++] = value;
  output[output_size] = '\0';
  if (!nested && strcmp(mode, "nested") == 0 && value == '\n' &&
      strstr(output, " mmfar=") != NULL) {
    nested = true;
    /* The nested entry has an unreadable stack. It must return before
     * accessing it and leave the original record's scratch intact.
     */
#if H2_TEST_CORRECTED
    h2_bk_cp_fault_capture(99u, exc_return, 1u);
#endif
  }
}

static void early_output_required(void) {
  assert(fpscr_reads == 0u);
  if (strcmp(mode, "uart-full") == 0) {
    assert(output_size == 0u && raw_polls > 0u && raw_polls < 3000000u);
  } else {
    assert(strstr(output, "H2_BK_CP_FAULT_ENTER reason=") != NULL);
    assert(strstr(output, "H2_BK_CP_FAULT reason=") != NULL);
    assert(strstr(output, " pc=0x02212346") != NULL);
    assert(strstr(output, " lr=0x02223457") != NULL);
    assert(strstr(output, " r0=0x00012300") != NULL);
    assert(strstr(output, " r3=0x00012303") != NULL);
    assert(strstr(output, " r12=0x00012304") != NULL);
  }
}

static int arch_is_enter_exception(void) {
  early_output_required();
  if (strcmp(mode, "trap-guard") == 0 || strcmp(mode, "nmi-guard") == 0)
    longjmp(escape, 2);
  return 0;
}

static int arch_is_ap_in_dump_mode(void) {
  early_output_required();
  return strcmp(mode, "nmi-ap") == 0;
}

static uint32_t sys_get_reboot_tag(void) {
  early_output_required();
  if (strcmp(mode, "nmi-reboot") == 0) longjmp(escape, 3);
  return 0u;
}

static void aon_pmu_drv_wdt_change_not_rosc_clk(void) {
  early_output_required();
}
static void aon_pmu_drv_wdt_rst_dev_enable(void) { longjmp(escape, 5); }
static void bk_wdt_force_feed(void) {
  early_output_required();
  ++watchdog_feeds;
}
static void arch_set_enter_exception(void) {}
static void dump_prologue(void) {}
static void dump_epilogue(void) {}
static void rtos_dump_system(void) {}
static void bk_misc_set_reset_reason(uint32_t reason) { (void)reason; }
static void bk_wdt_force_reboot(void) { longjmp(escape, 9); }
static uint32_t port_disable_interrupts_flag(void) {
  uint32_t old = masked;
  masked = 1u;
  return old;
}
static void port_enable_interrupts_flag(uint32_t before) { masked = before; }
static uint32_t __get_MSP(void) { return main_address; }
static uint32_t __get_PSP(void) { return process_address; }
static uint32_t __get_PRIMASK(void) { return masked; }
static uint32_t __get_BASEPRI(void) { return 0u; }
static uint32_t __get_FAULTMASK(void) { return 0u; }
static uint32_t __get_FPSCR(void) {
  ++fpscr_reads;
  return 0x1234u;
}

static uint32_t* test_context_stack(uint32_t address) {
  ++context_reads;
  if (strcmp(mode, "invalid-frame") == 0) longjmp(escape, 4);
  assert(address == main_address || address == process_address);
  return address == main_address ? main_stack + 8u : process_stack;
}

static void arch_dump_cpu_registers(uint32_t cause, SAVED_CONTEXT* context) {
  (void)cause;
  assert(context->pc == 0x02212346u && context->lr == 0x02223457u);
  ++normal_dumps;
}

static void dump_system_info(uint32_t reason, uint32_t lr, uint32_t sp) {
  user_except_handler_ex(reason, lr, sp);
}

/* SDK_FUNCTIONS */

int main(int argc, char** argv) {
  assert(argc == 4);
  mode = argv[1];
  exc_return = (uint32_t)strtoul(argv[2], NULL, 0);
  alignment = (uint32_t)strtoul(argv[3], NULL, 0);
  uint32_t* frame = (exc_return & 4u) ? process_stack : main_stack + 8u;
  for (unsigned i = 0u; i < 5u; ++i) frame[i] = 0x12300u + i;
  frame[5] = 0x02223457u;
  frame[6] = 0x02212346u;
  frame[7] = 0x01000000u | (alignment ? 1u << 9u : 0u);
  const int result = setjmp(escape);
  if (result == 0) {
    if (strncmp(mode, "trap", 4u) == 0)
      user_except_handler_ex(17u, exc_return, main_address);
    else
      user_nmi_handler(exc_return, main_address);
    abort();
  }
  if (strcmp(mode, "invalid-frame") == 0) {
    assert(result == 4 && strstr(output, "H2_BK_CP_FAULT_ENTER") != NULL);
    assert(strstr(output, "H2_BK_CP_FAULT reason=") == NULL);
    assert(fpscr_reads == 0u);
  } else {
    assert(result == 2 || result == 3 || result == 5 || result == 9);
    if (strcmp(mode, "uart-full") != 0) {
      uint32_t stack = (exc_return & 4u) ? process_address : main_address;
      stack += (8u + ((exc_return & 16u) ? 0u : 18u)) * 4u;
      stack += alignment ? 4u : 0u;
      char expected[40];
      snprintf(expected, sizeof(expected), " sp=0x%08x", stack);
      assert(strstr(output, expected) != NULL);
      assert(strstr(output, " cfsr=0x00000082") != NULL);
    }
    if (result == 9) {
      assert(normal_dumps == 1u && fpscr_reads == 1u);
      assert(watchdog_feeds == (strncmp(mode, "trap", 4u) == 0 ? 0u : 1u));
    } else {
      assert(normal_dumps == 0u && fpscr_reads == 0u);
    }
  }
  if (strcmp(mode, "nested") == 0) assert(nested);
  assert(h2_bk_cp_fault_context_enter(99u, 0u, 1u) == 0);
  return 0;
}
