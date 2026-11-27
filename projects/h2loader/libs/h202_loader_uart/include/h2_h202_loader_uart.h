#ifndef H2_H202_LOADER_UART_H
#define H2_H202_LOADER_UART_H

#include "h2_loader_command.h"
#include "h2_runtime.h"

/* Starts the single H202 UART worker. Runtime, command and the dependencies
 * borrowed by the copied config must remain alive until the device reboots. */
int h2_h202_loader_uart_start(h2_runtime_t* runtime,
                              h2_loader_command_t* command,
                              const h2_loader_command_config_t* config);

#endif
