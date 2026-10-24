#ifndef H2_BK_FAULT_TEST_SDK_H
#define H2_BK_FAULT_TEST_SDK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t CFSR, HFSR, BFAR, MMFAR;
} test_scb_t;
extern test_scb_t test_scb;
#define SCB (&test_scb)
uint32_t rtos_get_core_id(void);
uint32_t test_logical_core_id(void);
#define portGET_CORE_ID() test_logical_core_id()
uint32_t __get_PRIMASK(void);
uintptr_t test_fault_callsite(void);
#define H2_BK_FAULT_CALLSITE() test_fault_callsite()
int emergency_uart_write_buf(uint32_t port, const char* data, uint16_t length);

#endif
