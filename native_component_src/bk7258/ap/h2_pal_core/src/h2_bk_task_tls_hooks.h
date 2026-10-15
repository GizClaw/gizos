#ifndef H2_BK_TASK_TLS_HOOKS_H
#define H2_BK_TASK_TLS_HOOKS_H

#include "FreeRTOS.h"
#include "h2_bk_task_tls.h"

/* Expand the existing port hook here before redefining it, preserving
 * TrustZone secure-context cleanup as well as the new task TLS cleanup. */
static inline void h2_bk_task_tls_port_cleanup(void *task) {
    h2_bk_task_tls_cleanup(task);
    portCLEAN_UP_TCB(task);
}

#undef portCLEAN_UP_TCB
#define portCLEAN_UP_TCB(task) h2_bk_task_tls_port_cleanup((void *)(task))

#endif
