#ifndef H2_BK_PM_OBSERVER_H
#define H2_BK_PM_OBSERVER_H

#include <stdint.h>
#include "h2_bk_pm_trace.h"

/* Private, temporary diagnostic state captured before any fault-only UART
 * repair. No fields are written to Flash or used to choose a power state. */
typedef struct {
  uint32_t stage;
  uint32_t uart_tx_gpio_before;
  uint32_t uart_tx_gpio_saved;
  uint32_t uart_snapshot_valid;
  uint32_t low_voltage_calls;
  uint32_t aon_feeds;
} h2_bk_pm_fault_observation_t;

extern volatile h2_bk_pm_fault_observation_t h2_bk_pm_fault_observation;
void h2_bk_pm_fault_uart_restore(void);
void h2_bk_pm_trace_event(uint32_t event, uint32_t value);
void h2_bk_pm_vector_install(void);

#endif
