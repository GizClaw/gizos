/* Real SDK exception entries and register capture are inserted below. The
 * context-memory accessor models Cortex-M addresses on a 64-bit host. */
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "h2_bk_fault_context.h"
#include "h2_bk_fault_test_sdk.h"

extern void h2_bk_fault_context_observe(uint32_t reason,
                                        const SAVED_CONTEXT* context)
    __attribute__((weak));
extern int h2_bk_fault_context_enter(uint32_t reason, uint32_t exc_return,
                                     uint32_t stack) __attribute__((weak));

#define CONFIG_DEBUG_VERSION 0
#define CONFIG_DUMP_ENABLE 1
#define CONFIG_DUMP_INTO_FLASH 0
#define CONFIG_CM_BACKTRACE 0
#define CONFIG_INTERRUPT_DEBUG_RECORDER 0
#define CONFIG_AON_PMU_REG0_REFACTOR_DEV 0
#define CHECK_TASK_WDT_INTERRUPT 0x13u
#define CPU_ID rtos_get_core_id()

static jmp_buf escape;
static unsigned sequence, raw_sequence, rendezvous_sequence;
static unsigned core, masked, raw_count, flush_count;
static unsigned fpscr_reads, normal_dumps;
static unsigned context_reads;
static bool normal_profile;
static bool stop_at_flush;
static bool invalid_frame;
static SAVED_CONTEXT normal_context;
static char raw_output[1200];
static size_t raw_length;
static uint32_t main_stack[32], process_stack[32];
static const uint32_t main_address = 0x28001020u;
static const uint32_t process_address = 0x60001000u;
uint32_t g_wdt_handler_lr;
volatile const uint8_t build_version[] = "test";
test_scb_t test_scb = {0x01000000u, 0x40000000u, 0x60012345u, 0x28012345u};

uint32_t rtos_get_core_id(void) { return core + 1u; }
uint32_t test_logical_core_id(void) { return core; }
uint32_t __get_PRIMASK(void) { return masked; }
uintptr_t test_fault_callsite(void) { return 0x0200abcdu; }
int emergency_uart_write_buf(uint32_t port, const char* data, uint16_t size) {
  assert(port == CONFIG_DUMP_UART_PRINT_PORT &&
         raw_length + size < sizeof(raw_output));
  memcpy(raw_output + raw_length, data, size);
  raw_length += size;
  raw_output[raw_length] = '\0';
  raw_sequence = ++sequence;
  ++raw_count;
  return 1;
}
int shell_assert_out(bool keep_running, const char* format, ...) {
  (void)keep_running;
  (void)format;
  return 1;
}
uint32_t rtos_disable_int(void) {
  uint32_t before = masked;
  masked = 1u;
  return before;
}
void rtos_enable_int(uint32_t before) { masked = before; }
uint32_t rtos_enter_critical(void) { return rtos_disable_int(); }
static uint32_t port_disable_interrupts_flag(void) {
  return rtos_disable_int();
}
static void port_enable_interrupts_flag(uint32_t before) {
  rtos_enable_int(before);
}
static int arch_is_enter_exception(void) { return 0; }
static void arch_set_enter_exception(void) {
  rendezvous_sequence = ++sequence;
  longjmp(escape, 2);
}
static void dump_prologue(void) {}
static void dump_epilogue(void) {}
static void rtos_dump_system(void) {}
static void bk_misc_set_reset_reason(uint32_t reason) { (void)reason; }
static void bk_wdt_force_reboot(void) { longjmp(escape, 9); }
static uint32_t rtos_get_time(void) { return 42u; }
static uint32_t __get_MSP(void) { return main_address; }
static uint32_t __get_PSP(void) { return process_address; }
#define __get_R0() 0u
#define __get_R1() 1u
#define __get_R2() 2u
#define __get_R3() 3u
#define __get_R4() 4u
#define __get_R5() 5u
#define __get_R6() 6u
#define __get_R7() 7u
#define __get_R8() 8u
#define __get_R9() 9u
#define __get_R10() 10u
#define __get_R11() 11u
#define __get_R12() 12u
#define __get_SP() 0x28002000u
#define __get_LR() 0x02002345u
#define __get_PC() 0x02003457u
#define __get_xPSR() 0x01000003u
#define __get_BASEPRI() 0u
#define __get_FAULTMASK() 0u
#define __get_CONTROL() 0u
static uint32_t __get_FPSCR(void) {
  if (normal_profile) assert(context_reads == 8u);
  ++fpscr_reads;
  return 0x1234u;
}

static uint32_t* test_context_stack(uint32_t address) {
  ++context_reads;
  if (invalid_frame) longjmp(escape, 4);
  assert(address == main_address || address == process_address);
  return address == main_address ? main_stack + 8u : process_stack;
}
static void arch_dump_cpu_registers(uint32_t cause, SAVED_CONTEXT* context) {
  (void)cause;
  normal_context = *context;
  ++normal_dumps;
}

#define RESET_SOURCE_CRASH_ASSERT 7u
#include "common/bk_assert.h"

typedef uint32_t u32;
typedef struct log_device log_device_t;
typedef struct {
  int (*io_ctrl)(log_device_t*, unsigned, void*);
} log_driver_t;
struct log_device {
  log_driver_t* dev_drv;
};
#define SHELL_IO_CTRL_FLUSH 42u
static int log_control(log_device_t* device, unsigned command,
                       void* parameter) {
  (void)device;
  (void)parameter;
  assert(command == SHELL_IO_CTRL_FLUSH);
  ++flush_count;
  if (stop_at_flush) longjmp(escape, 3);
  return 0;
}
static log_driver_t log_driver = {log_control};
static log_device_t log_device = {&log_driver};
static log_device_t* log_dev = &log_device;

/* SDK_FUNCTIONS */

/* Mach-O has no GNU --wrap option. Model that call redirection here; the
 * native CMake test and ARM link separately verify the real linker wiring. */
void test_selected_flush(void) {
#if H2_TEST_CORRECTED
  __wrap_shell_log_flush();
#else
  __real_shell_log_flush();
#endif
}

static uint32_t value(const char* line, const char* field) {
  char key[64];
  assert(strlen(field) + 3u < sizeof(key));
  (void)snprintf(key, sizeof(key), " %s=", field);
  const char* position = strstr(line, key);
  assert(position != NULL);
  return (uint32_t)strtoul(position + strlen(key), NULL, 16);
}

int main(int argc, char** argv) {
  assert(argc == 5);
  const bool broken_frame = strcmp(argv[1], "invalid-frame") == 0;
  const bool normal_dump = strcmp(argv[1], "context-normal") == 0;
  normal_profile = normal_dump;
  const bool exception = strcmp(argv[1], "fault") == 0 || broken_frame;
  const uint32_t exc_return = (uint32_t)strtoul(argv[2], NULL, 16);
  const bool aligned = atoi(argv[3]) != 0;
  core = (unsigned)atoi(argv[4]);
  const uint32_t frame[8] = {
      0x60012340u, 0x28012340u,
      2u,          3u,
      12u,         0x02005679u,
      0x02001235u, 0x01000000u | (aligned ? 0x200u : 0u)};
  memcpy(main_stack + 8u, frame, sizeof(frame));
  memcpy(process_stack, frame, sizeof(frame));
  for (unsigned i = 0u; i < 8u; ++i) main_stack[i] = 4u + i;
  stop_at_flush = strcmp(argv[1], "assert-flush") == 0;
  int stopped = 0;
  switch (setjmp(escape)) {
    case 0:
      if (normal_dump) {
        dump_context(exc_return, main_address);
        assert(normal_dumps == 1u && fpscr_reads == 1u && raw_count == 0u);
        assert((uint32_t)normal_context.pc == frame[6]);
        assert((uint32_t)normal_context.lr == frame[5]);
        assert((uint32_t)normal_context.r4 == 4u);
        assert((uint32_t)normal_context.fpscr == 0x1234u);
        puts("PASS original SDK context dump preserved");
        return 0;
      }
      invalid_frame = broken_frame;
      if (exception)
        user_except_handler_ex(3u, exc_return, main_address);
      else
        BK_ASSERT(false);
      abort();
    case 2:
      stopped = 2;
      break;
    case 3:
      stopped = 3;
      break;
    case 4:
      stopped = 4;
      break;
    default:
      abort();
  }
  if (broken_frame) {
    if (raw_count != 1u) {
      fprintf(stderr, "missing entry record before invalid SDK frame read\n");
      return 2;
    }
    assert(stopped == 4);
    assert(strstr(raw_output, "H2_BK_NATIVE_FAULT_ENTER ") != NULL);
    assert(strstr(raw_output, "H2_BK_NATIVE_FAULT ") == NULL);
    puts("PASS entry recorded before unreadable frame");
    return 0;
  }
  if (exception) {
    if (raw_count != 2u) {
      fprintf(stderr, "missing early fault record before SDK rendezvous\n");
      return 2;
    }
    assert(stopped == 2 && raw_sequence < rendezvous_sequence);
    assert(masked == 1u);
    assert(fpscr_reads == 0u);
    const char* entry = strstr(raw_output, "H2_BK_NATIVE_FAULT_ENTER ");
    const char* fault = strstr(raw_output, "H2_BK_NATIVE_FAULT ");
    assert(entry != NULL && fault != NULL && entry < fault);
    assert(value(entry, "stack") == main_address);
    const uint32_t base = (exc_return & 4u) ? process_address : main_address;
    const uint32_t adjustment =
        32u + ((exc_return & 16u) ? 0u : 72u) + (aligned ? 4u : 0u);
    assert(value(fault, "pc") == frame[6]);
    assert(value(fault, "lr") == frame[5]);
    assert(value(fault, "sp") == base + adjustment);
    assert(value(fault, "r0") == frame[0]);
    assert(value(fault, "r1") == frame[1]);
    assert(value(fault, "r2") == frame[2]);
    assert(value(fault, "r3") == frame[3]);
    assert(value(fault, "r12") == frame[4]);
    assert(value(fault, "exc_return") == exc_return);
    assert(value(fault, "xpsr") == frame[7]);
  } else {
    const char* flush = strstr(raw_output, "H2_BK_LOG_FLUSH_MASKED");
    if (flush == NULL) {
      fprintf(stderr,
              "missing masked-flush record before SDK assertion dump\n");
      return 2;
    }
    assert(flush_count == 1u && masked == 1u);
    assert(value(flush, "caller") == 0x0200abcdu);
    if (stop_at_flush) {
      assert(stopped == 3 && raw_count == 1u);
    } else {
      const char* fault = strstr(raw_output, "H2_BK_NATIVE_FAULT ");
      assert(stopped == 2 && raw_count == 3u && fault != NULL);
      assert(value(fault, "reason") == RESET_SOURCE_CRASH_ASSERT);
      assert(value(fault, "pc") == __get_PC());
      assert(raw_sequence < rendezvous_sequence);
    }
  }
  printf("PASS %s core%u EXC_RETURN=%08x align=%u\n", argv[1], core,
         (unsigned)exc_return, aligned);
  return 0;
}
