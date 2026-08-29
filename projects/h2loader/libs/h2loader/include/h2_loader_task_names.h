#ifndef H2_LOADER_TASK_NAMES_H
#define H2_LOADER_TASK_NAMES_H

#define H2_LOADER_APP_COMMAND_TASK_NAME_VALUE "$h2loader/appcmd"
#define H2_LOADER_RETURN_TASK_NAME_VALUE "$h2loader/return"
#define H2_LOADER_BLE_LINK_TASK_NAME_VALUE "$h2loader/blelink"
#define H2_LOADER_UART_COMMAND_TASK_NAME_VALUE "$h2loader/uartcmd"

#ifdef __cplusplus
extern "C" {
#endif

extern const char h2_loader_app_command_task_name[sizeof(
    H2_LOADER_APP_COMMAND_TASK_NAME_VALUE)];
extern const char
    h2_loader_return_task_name[sizeof(H2_LOADER_RETURN_TASK_NAME_VALUE)];
extern const char
    h2_loader_ble_link_task_name[sizeof(H2_LOADER_BLE_LINK_TASK_NAME_VALUE)];
extern const char h2_loader_uart_command_task_name[sizeof(
    H2_LOADER_UART_COMMAND_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
