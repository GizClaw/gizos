#ifndef H2_BK_FAULT_CONTEXT_H
#define H2_BK_FAULT_CONTEXT_H

#include <stdint.h>

#include "stack_base.h"

/* Called synchronously with local interrupts masked, before the generated
 * SDK fatal path's peer-core rendezvous. The context is borrowed for the call.
 */
int h2_bk_fault_context_enter(uint32_t reason, uint32_t exc_return,
                              uint32_t stack);
void h2_bk_fault_context_observe(uint32_t reason, const SAVED_CONTEXT* context);

/* SDK assertions flush logs before entering bk_system_dump(). Preserve that
 * operation while recording callers that have already masked interrupts. */
void __wrap_shell_log_flush(void);

#endif
