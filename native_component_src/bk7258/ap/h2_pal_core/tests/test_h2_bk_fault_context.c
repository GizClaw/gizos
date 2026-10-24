#include "h2_bk_fault_context.h"
#include "h2_bk_fault_test_sdk.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

test_scb_t test_scb = {0x01000000u, 0x40000000u, 0x60012345u, 0x28012345u};
static uint32_t s_core, s_primask;
static unsigned s_writes, s_flushes;
static char s_output[512];
static bool s_reenter;
static SAVED_CONTEXT s_context;

uint32_t rtos_get_core_id(void) { return s_core + 1u; }
uint32_t test_logical_core_id(void) { return s_core; }
uint32_t __get_PRIMASK(void) { return s_primask; }
uintptr_t test_fault_callsite(void) { return 0x02001235u; }

int emergency_uart_write_buf(uint32_t port, const char* data, uint16_t length) {
  assert(port == 1u);
  assert(length < sizeof(s_output));
  memcpy(s_output, data, length);
  s_output[length] = '\0';
  ++s_writes;
  if (s_reenter) {
    s_reenter = false;
    h2_bk_fault_context_observe(9u, &s_context);
  }
  return 1;
}

void __real_shell_log_flush(void) { ++s_flushes; }

static uint32_t value(const char* name) {
  char key[64];
  const size_t size = strlen(name);
  assert(size + 3u < sizeof(key));
  key[0] = ' ';
  memcpy(key + 1u, name, size);
  memcpy(key + size + 1u, "=", 2u);
  const char* found = strstr(s_output, key);
  assert(found != NULL);
  return (uint32_t)strtoul(found + strlen(key), NULL, 16);
}

int main(void) {
  s_context = (SAVED_CONTEXT){
      .r0 = 0x60001200u,
      .r1 = 0x28000080u,
      .r2 = 7u,
      .r3 = 8u,
      .r12 = 12u,
      .sp = 0x60001280u,
      .lr = 0x02002345u,
      .pc = 0x02003457u,
      .xpsr = 0x01000000u,
      .primask = 1u,
      .control = 0xfffffffdu,
  };
  h2_bk_fault_context_observe(3u, NULL);
  s_core = 2u;
  h2_bk_fault_context_observe(3u, &s_context);
  assert(s_writes == 0u);

  s_core = 0u;
  s_reenter = true;
  h2_bk_fault_context_observe(3u, &s_context);
  assert(s_writes == 1u && s_flushes == 0u);
  assert(strstr(s_output, "H2_BK_NATIVE_FAULT") != NULL);
  assert(value("reason") == 3u && value("core") == 0u);
  assert(value("pc") == 0x02003457u && value("lr") == 0x02002345u);
  assert(value("sp") == 0x60001280u && value("r0") == 0x60001200u);
  assert(value("r1") == 0x28000080u && value("r2") == 7u);
  assert(value("r3") == 8u && value("r12") == 12u);
  assert(value("exc_return") == 0xfffffffdu);
  assert(value("primask") == 1u && value("xpsr") == 0x01000000u);
  assert(value("cfsr") == test_scb.CFSR);
  assert(value("hfsr") == test_scb.HFSR);
  assert(value("bfar") == test_scb.BFAR);
  assert(value("mmfar") == test_scb.MMFAR);
  const size_t length = strlen(s_output);
  assert(length >= 2u && strcmp(s_output + length - 2u, "\r\n") == 0);

  h2_bk_fault_context_observe(4u, &s_context);
  assert(s_writes == 1u);
  s_core = 1u;
  s_context.pc = -1;
  s_context.lr = -1;
  h2_bk_fault_context_observe(0xffffffffu, &s_context);
  assert(s_writes == 2u && value("core") == 1u);
  assert(value("pc") == 0xffffffffu && value("lr") == 0xffffffffu);
  assert(value("reason") == 0xffffffffu);

  __wrap_shell_log_flush();
  assert(s_flushes == 1u && s_writes == 2u && s_primask == 0u);
  s_primask = 1u;
  __wrap_shell_log_flush();
  assert(s_flushes == 2u && s_writes == 3u && s_primask == 1u);
  assert(strstr(s_output, "H2_BK_LOG_FLUSH_MASKED") != NULL);
  assert(value("caller") == 0x02001235u);
  __wrap_shell_log_flush();
  assert(s_flushes == 3u && s_writes == 4u);
  h2_bk_fault_context_enter(3u, 0xfffffffdu, 0x60001200u);
  assert(s_writes == 5u);
  assert(strstr(s_output, "H2_BK_NATIVE_FAULT_ENTER") != NULL);
  assert(value("stack") == 0x60001200u);
  assert(value("exc_return") == 0xfffffffdu);
  h2_bk_fault_context_enter(3u, 0xfffffffdu, 0x60001200u);
  assert(s_writes == 5u);
  return 0;
}
