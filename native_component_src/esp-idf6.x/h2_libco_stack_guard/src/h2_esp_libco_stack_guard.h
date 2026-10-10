#ifndef H2_ESP_LIBCO_STACK_GUARD_H
#define H2_ESP_LIBCO_STACK_GUARD_H
#include <stdint.h>
void h2_esp_libco_stack_guard_link(void);
uint32_t h2_libco_riscv32_stack_prepare(uint32_t stack_min, uint32_t stack_max);
uint32_t h2_libco_riscv32_stack_switch_enter(
    uint32_t next_min, uint32_t next_max,
    uint32_t *previous_min, uint32_t *previous_max);
void h2_libco_riscv32_stack_switch_exit(uint32_t token);
#endif
