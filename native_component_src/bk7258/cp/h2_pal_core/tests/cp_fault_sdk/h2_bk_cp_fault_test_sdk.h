#ifndef H2_BK_CP_FAULT_TEST_SDK_H
#define H2_BK_CP_FAULT_TEST_SDK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t CFSR, HFSR, BFAR, MMFAR;
} h2_cp_test_scb_t;

extern h2_cp_test_scb_t test_scb;
#define SCB (&test_scb)
bool h2_bk_cp_test_uart_full(void);
void h2_bk_cp_test_uart_putc(char value);

#endif
