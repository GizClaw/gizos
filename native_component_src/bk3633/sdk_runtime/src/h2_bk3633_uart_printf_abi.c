#include "h2_bk3633_uart_printf_abi.h"

/*
 * Pinned vendor SDK ABI referenced directly by the selected RWIP sources.
 * sdk_runtime is its sole owner. Keep it strong and silent so image logging
 * cannot run from an IRQ or RWIP scheduler deep-stack context.
 */
int uart_printf(const char *format, ...)
{
    (void)format;
    return 0;
}
