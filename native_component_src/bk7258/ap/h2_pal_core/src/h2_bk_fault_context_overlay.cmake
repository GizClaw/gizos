# Keep the sealed SDK read-only. Its register-capture body remains the source
# of truth for the Cortex-M/FPU exception frame, including stack alignment.
file(READ "${H2_BK_FAULT_INPUT}" h2_fault_source)
string(REPLACE "\r\n" "\n" h2_fault_source "${h2_fault_source}")
string(SHA256 h2_fault_sha256 "${h2_fault_source}")
if(NOT h2_fault_sha256 STREQUAL "59df3349863e398d57779e1cec4104cd9ebe006e29ff2c627545fe6bd63793f9")
  message(FATAL_ERROR "Pinned BK trap source changed; revalidate fault diagnostics")
endif()

function(h2_bk_fault_replace signature replacement)
  string(REPLACE "${signature}" "" without "${h2_fault_source}")
  string(LENGTH "${h2_fault_source}" before)
  string(LENGTH "${without}" after)
  string(LENGTH "${signature}" width)
  math(EXPR removed "${before} - ${after}")
  if(NOT removed EQUAL width)
    message(FATAL_ERROR "Pinned BK trap signature changed; revalidate fault diagnostics")
  endif()
  string(REPLACE "${signature}" "${replacement}" changed "${h2_fault_source}")
  set(h2_fault_source "${changed}" PARENT_SCOPE)
endfunction()

set(h2_capture_prefix [=[static void dump_context(uint32_t lr, uint32_t msp)
{
    SAVED_CONTEXT regs;
]=])
set(h2_capture_end "    arch_dump_cpu_registers(0, &regs);")
string(FIND "${h2_fault_source}" "${h2_capture_prefix}" h2_capture_start)
string(FIND "${h2_fault_source}" "${h2_capture_end}" h2_capture_stop)
if(h2_capture_start LESS 0 OR h2_capture_stop LESS h2_capture_start)
  message(FATAL_ERROR "Pinned BK context capture changed")
endif()
string(LENGTH "${h2_capture_prefix}" h2_capture_prefix_length)
math(EXPR h2_body_start "${h2_capture_start} + ${h2_capture_prefix_length}")
math(EXPR h2_body_length "${h2_capture_stop} - ${h2_body_start}")
string(SUBSTRING "${h2_fault_source}" ${h2_body_start} ${h2_body_length} h2_capture_body)
string(REPLACE "regs." "regs->" h2_capture_pointer_body "${h2_capture_body}")
# Reading FPSCR can itself fault when the original exception reports NOCP.
# Early diagnostics need only the already stacked integer registers. The
# normal SDK dump still reads and reports FPSCR at its original stage.
string(REPLACE "    regs->fpscr = __get_FPSCR();"
  "    regs->fpscr = read_fpscr ? __get_FPSCR() : 0;"
  h2_capture_pointer_body "${h2_capture_pointer_body}")
set(h2_capture_helper "static void h2_bk_capture_fault_context(uint32_t lr, uint32_t msp, SAVED_CONTEXT *regs, int read_fpscr)\n{\n${h2_capture_pointer_body}}\n\n")
set(h2_capture_replacement "${h2_capture_helper}${h2_capture_prefix}    h2_bk_capture_fault_context(lr, msp, &regs, 1);\n\n")
h2_bk_fault_replace("${h2_capture_prefix}${h2_capture_body}" "${h2_capture_replacement}")

set(h2_signature "#include \"spinlock.h\"")
set(h2_replacement [=[#include "spinlock.h"

extern void h2_bk_fault_context_observe(uint32_t reason,
    const SAVED_CONTEXT *context) __attribute__((weak));
extern void h2_bk_fault_context_enter(uint32_t reason, uint32_t exc_return,
    uint32_t stack) __attribute__((weak));]=])
h2_bk_fault_replace("${h2_signature}" "${h2_replacement}")

set(h2_signature [=[void user_except_handler_ex(uint32_t reset_reason, uint32_t lr, uint32_t sp)
{
    if (0 == arch_is_enter_exception()) {
        // Make sure the interrupt is disable
        uint32_t int_level = port_disable_interrupts_flag();]=])
set(h2_replacement [=[void user_except_handler_ex(uint32_t reset_reason, uint32_t lr, uint32_t sp)
{
    if (0 == arch_is_enter_exception()) {
        // Make sure the interrupt is disable
        uint32_t int_level = port_disable_interrupts_flag();

        if (h2_bk_fault_context_enter != NULL) {
            h2_bk_fault_context_enter(reset_reason, lr, sp);
        }
        if (h2_bk_fault_context_observe != NULL) {
            SAVED_CONTEXT early_context;
            h2_bk_capture_fault_context(lr, sp, &early_context, 0);
            h2_bk_fault_context_observe(reset_reason, &early_context);
        }]=])
h2_bk_fault_replace("${h2_signature}" "${h2_replacement}")

set(h2_signature "    SAVED_CONTEXT regs = {0};\n    store_cpu_regs(ECAUSE_ASSERT, &regs);")
set(h2_replacement [=[    SAVED_CONTEXT regs = {0};
    if (h2_bk_fault_context_enter != NULL) {
        h2_bk_fault_context_enter(RESET_SOURCE_CRASH_ASSERT, 0, 0);
    }
    store_cpu_regs(ECAUSE_ASSERT, &regs);]=])
h2_bk_fault_replace("${h2_signature}" "${h2_replacement}")

set(h2_signature "    store_cpu_regs(ECAUSE_ASSERT, &regs);")
set(h2_replacement [=[    store_cpu_regs(ECAUSE_ASSERT, &regs);

    if (h2_bk_fault_context_observe != NULL) {
        h2_bk_fault_context_observe(RESET_SOURCE_CRASH_ASSERT, &regs);
    }]=])
h2_bk_fault_replace("${h2_signature}" "${h2_replacement}")

file(WRITE "${H2_BK_FAULT_OUTPUT}" "${h2_fault_source}")
