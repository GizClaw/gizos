#include "pwm.h"

int main(void)
{
    PWM_DRV_DESC descriptor = {
        .channel = 0u,
        .en = 0u,
        .int_en = 0u,
        .mode = PWM_MODE_IDLE,
        .cpedg_sel = 1u,
        .contiu_mode = 0u,
        .clk_src = PWM_CLK_XTAL16M,
        .pre_divid = 0u,
        .end_value = 2u,
        .duty_cycle = 0u,
        .p_Int_Handler = 0,
    };
    void (*init0)(PWM_DRV_DESC *) = pwm0_init;
    void (*init1)(PWM_DRV_DESC *) = pwm1_init;
    return descriptor.channel != 0u || init0 == 0 || init1 == 0 ? 1 : 0;
}
