#include "i2c.h"

int main(void) {
    STATUS (*read_registers)(uint8_t, uint8_t, uint8_t *, uint8_t) = i2c_read;
    STATUS (*write_registers)(uint8_t, uint8_t, uint8_t *, uint8_t) = i2c_write;
    return read_registers == 0 || write_registers == 0;
}
