#include "adc.h"
#include "gpio.h"

int main(void) {
    void (*init)(uint8_t, uint8_t) = adc_init;
    void (*deinit)(uint8_t) = adc_deinit;
    uint8_t (*read_gpio)(uint8_t) = gpio_get_input;
    volatile unsigned long *config = &SADC_REG0X0_CFG0;
    volatile unsigned long *data = &SADC_REG0X4_DAT;
    return init == 0 || deinit == 0 || read_gpio == 0 || config == 0 ||
           data == 0;
}
