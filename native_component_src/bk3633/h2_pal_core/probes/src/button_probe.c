#include "gpio.h"

int main(void)
{
    uint8_t (*read_input)(uint8_t) = gpio_get_input;
    void (*configure)(uint8_t, Dir_Type, Pull_Type) = gpio_config;
    return read_input == 0 || configure == 0 ? 1 : 0;
}
