# Only generated build inputs change. Integer frame interpretation continues
# to come from the sealed SDK, including MSP/PSP and floating-frame sizing.
file(READ "${H2_BK_CP_TRAP_INPUT}" h2_cp_trap)
file(READ "${H2_BK_CP_STARTUP_INPUT}" h2_cp_startup)
foreach(h2_name trap startup)
  string(REPLACE "\r\n" "\n" h2_cp_${h2_name} "${h2_cp_${h2_name}}")
  string(SHA256 h2_cp_${h2_name}_sha "${h2_cp_${h2_name}}")
endforeach()
if(NOT h2_cp_trap_sha STREQUAL "3ee75295785b6da57fd27f0ac64dabaed56fa81b6e54d02a7ff02645f9195c16"
    OR NOT h2_cp_startup_sha STREQUAL "528b6a01b88604b4cf7e10dbe61832e6aa00346498b483007141d0755d603d53")
  message(FATAL_ERROR "Pinned CP exception sources changed; revalidate early context")
endif()

function(h2_bk_cp_fault_replace variable signature replacement)
  string(REPLACE "${signature}" "" without "${${variable}}")
  string(LENGTH "${${variable}}" before)
  string(LENGTH "${without}" after)
  string(LENGTH "${signature}" width)
  math(EXPR removed "${before} - ${after}")
  if(NOT removed EQUAL width)
    message(FATAL_ERROR "Pinned CP exception signature changed")
  endif()
  string(REPLACE "${signature}" "${replacement}" changed "${${variable}}")
  set(${variable} "${changed}" PARENT_SCOPE)
endfunction()

set(h2_cp_prefix [=[static void dump_context(uint32_t lr, uint32_t msp)
{
    SAVED_CONTEXT regs;
]=])
set(h2_cp_end "    arch_dump_cpu_registers(0, &regs);")
string(FIND "${h2_cp_trap}" "${h2_cp_prefix}" h2_cp_start)
string(FIND "${h2_cp_trap}" "${h2_cp_end}" h2_cp_stop)
if(h2_cp_start LESS 0 OR h2_cp_stop LESS h2_cp_start)
  message(FATAL_ERROR "Pinned CP frame capture changed")
endif()
string(LENGTH "${h2_cp_prefix}" h2_cp_prefix_length)
math(EXPR h2_cp_body_start "${h2_cp_start} + ${h2_cp_prefix_length}")
math(EXPR h2_cp_body_size "${h2_cp_stop} - ${h2_cp_body_start}")
string(SUBSTRING "${h2_cp_trap}" ${h2_cp_body_start} ${h2_cp_body_size} h2_cp_body)
string(REPLACE "regs." "regs->" h2_cp_capture "${h2_cp_body}")
string(REPLACE "    regs->fpscr = __get_FPSCR();" "    regs->fpscr = 0;" h2_cp_capture "${h2_cp_capture}")
set(h2_cp_helper "static void __attribute__((section(\".iram\"), noinline)) h2_bk_cp_capture_frame(uint32_t lr, uint32_t msp, SAVED_CONTEXT *regs)\n{\n${h2_cp_capture}}\n\n")
string(APPEND h2_cp_helper [=[void __attribute__((section(".iram"), noinline)) h2_bk_cp_fault_capture(uint32_t reason, uint32_t lr, uint32_t sp)
{
    if (h2_bk_cp_fault_context_enter == NULL ||
        !h2_bk_cp_fault_context_enter(reason, lr, sp)) return;
    if (h2_bk_cp_fault_context_observe != NULL) {
        SAVED_CONTEXT context;
        h2_bk_cp_capture_frame(lr, sp, &context);
        h2_bk_cp_fault_context_observe(reason, &context);
    }
}

]=])
h2_bk_cp_fault_replace(h2_cp_trap "${h2_cp_prefix}" "${h2_cp_helper}${h2_cp_prefix}")
set(h2_cp_declarations [=[#include "stack_base.h"

extern int h2_bk_cp_fault_context_enter(uint32_t reason, uint32_t exc_return,
    uint32_t stack) __attribute__((weak));
extern void h2_bk_cp_fault_context_observe(uint32_t reason,
    const SAVED_CONTEXT *context) __attribute__((weak));]=])
h2_bk_cp_fault_replace(h2_cp_trap "#include \"stack_base.h\"" "${h2_cp_declarations}")

set(h2_cp_signature [=[void user_except_handler_ex(uint32_t reset_reason, uint32_t lr, uint32_t sp)
{]=])
set(h2_cp_replacement [=[void __attribute__((section(".iram"))) user_except_handler_ex(uint32_t reset_reason, uint32_t lr, uint32_t sp)
{
    h2_bk_cp_fault_capture(reset_reason, lr, sp);]=])
h2_bk_cp_fault_replace(h2_cp_trap "${h2_cp_signature}" "${h2_cp_replacement}")

set(h2_cp_signature [=[void user_nmi_handler(uint32_t lr, uint32_t sp)
{]=])
set(h2_cp_replacement [=[extern void h2_bk_cp_fault_capture(uint32_t reason, uint32_t lr,
    uint32_t sp) __attribute__((weak));

void __attribute__((section(".iram"))) user_nmi_handler(uint32_t lr, uint32_t sp)
{
    if (h2_bk_cp_fault_capture != NULL)
        h2_bk_cp_fault_capture(RESET_SOURCE_NMI_WDT, lr, sp);]=])
h2_bk_cp_fault_replace(h2_cp_startup "${h2_cp_signature}" "${h2_cp_replacement}")

file(WRITE "${H2_BK_CP_TRAP_OUTPUT}" "${h2_cp_trap}")
file(WRITE "${H2_BK_CP_STARTUP_OUTPUT}" "${h2_cp_startup}")
