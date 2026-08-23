#include "BK3633_RegList.h"

#include <stdint.h>

int main(void)
{
    volatile unsigned long *control = &addTRNG_Reg0x0;
    volatile unsigned long *data = &addTRNG_Reg0x1;
    uint32_t enabled = (uint32_t)get_TRNG_Reg0x0_trng_en;
    return (control == 0 || data == 0 || enabled > 1u) ? 1 : 0;
}
