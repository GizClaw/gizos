/* Header-only probe for the BK3633 SDK include contract. */
#include "rwip_config.h"
#include "arch.h"
#include "uart.h"
#include "ke_mem.h"
#include "rwip.h"

int main(void)
{
    return (CFG_ROLE == 4) ? 0 : 1;
}
