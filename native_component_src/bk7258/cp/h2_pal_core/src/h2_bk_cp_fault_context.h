#ifndef H2_BK_CP_FAULT_CONTEXT_H
#define H2_BK_CP_FAULT_CONTEXT_H

#include <stdint.h>

#include "stack_base.h"

/* Private fatal-path hooks. The CP exception context owns these calls;
 * enter returns zero on recursion, before any further stack inspection.
 */
int h2_bk_cp_fault_context_enter(uint32_t reason, uint32_t exc_return,
                                 uint32_t stack);
void h2_bk_cp_fault_context_observe(uint32_t reason,
                                    const SAVED_CONTEXT* context);

#endif
