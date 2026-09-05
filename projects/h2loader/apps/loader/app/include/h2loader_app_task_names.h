#ifndef H2LOADER_APP_TASK_NAMES_H
#define H2LOADER_APP_TASK_NAMES_H

#define H2LOADER_APP_COMMAND_TASK_NAME_VALUE "h2loader/appcmd"
#define H2LOADER_BLE_COMMAND_TASK_NAME_VALUE "h2loader/blecmd"

#ifdef __cplusplus
extern "C" {
#endif

extern const char h2loader_app_command_task_name[sizeof(
    H2LOADER_APP_COMMAND_TASK_NAME_VALUE)];
extern const char h2loader_ble_command_task_name[sizeof(
    H2LOADER_BLE_COMMAND_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
