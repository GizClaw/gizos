#include "icu.h"
#include "rwip.h"

int main(void) {
    uint8_t (*sleep_try)(void) = rwip_sleep;
    void (*cpu_sleep)(void) = cpu_reduce_voltage_sleep;
    void (*wake)(void) = cpu_wakeup;
    void (*deep)(void) = deep_sleep;
    void (*reboot)(void) = cpu_reset;
    return sleep_try == 0 || cpu_sleep == 0 || wake == 0 || deep == 0 ||
           reboot == 0;
}
