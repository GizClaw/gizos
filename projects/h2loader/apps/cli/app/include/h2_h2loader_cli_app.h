#ifndef H2_H2LOADER_CLI_APP_H
#define H2_H2LOADER_CLI_APP_H

#include "h2_command.h"
#include "h2_h2loader_host.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_h2loader_cli_stream {
    H2_H2LOADER_CLI_STREAM_STDOUT = 1,
    H2_H2LOADER_CLI_STREAM_STDERR = 2,
} h2_h2loader_cli_stream_t;

typedef enum h2_h2loader_cli_exit {
    H2_H2LOADER_CLI_EXIT_OK = 0,
    H2_H2LOADER_CLI_EXIT_USAGE = 2,
    H2_H2LOADER_CLI_EXIT_RUNTIME = 3,
} h2_h2loader_cli_exit_t;

/**
 * Returns a started BLE Host API, or NULL when BLE is unavailable. Called at
 * most once per process, and only after a command actually requests a BLE
 * transport; serial-only invocations never call it.
 */
typedef const h2_pal_ble_host_api_t *(*h2_h2loader_cli_ble_acquire_fn)(
    void *user);

typedef struct h2_h2loader_cli_config {
    int argc;
    const char *const *argv;
    const h2_pal_serial_host_api_t *serial;
    const h2_command_io_api_t *stdout_io;
    const h2_command_io_api_t *stderr_io;
    h2_h2loader_host_cancelled_fn is_cancelled;
    void *cancel_user;
    h2_h2loader_cli_ble_acquire_fn acquire_ble;
    void *ble_user;
} h2_h2loader_cli_config_t;

/** Blocking portable CLI App entry. Returns a process exit category. */
int h2_h2loader_cli_main(
    h2_runtime_t *runtime,
    const h2_h2loader_cli_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
