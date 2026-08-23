#include "h2_h2loader_cli_target.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t cancelled;

static void handle_signal(int signal_number) {
    (void)signal_number;
    cancelled = 1;
}

static int is_cancelled(void *user) {
    (void)user;
    return cancelled != 0;
}

static h2_pal_result_t stdio_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    FILE *stream = user;
    (void)timeout_ms;
    if (stream == NULL || out_written == NULL ||
        (buffer == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_written = fwrite(buffer, 1u, len, stream);
    return *out_written == len ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t stdio_flush(void *user) {
    return user != NULL && fflush((FILE *)user) == 0
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

/* Starting the BLE Host may trigger host Bluetooth permission enforcement
 * (macOS TCC aborts processes without an entitlement), so it stays deferred
 * until the App requests a BLE transport; serial-only runs never start it. */
static const h2_pal_ble_host_api_t *started_ble;
static int ble_start_attempted;

static const h2_pal_ble_host_api_t *acquire_ble(void *user) {
    const h2_pal_ble_host_api_t *ble = user;
    if (!ble_start_attempted) {
        ble_start_attempted = 1;
        if (h2_pal_ble_start(ble) == H2_PAL_OK) started_ble = ble;
    }
    return started_ble;
}

/* Options whose value is a host filesystem path typed by the user. */
static int is_path_option(const char *arg) {
    static const char *const options[] = {
        "--file", "--out", "--app-bin", "--data-dir",
    };
    for (size_t i = 0u; i < sizeof(options) / sizeof(options[0]); ++i) {
        if (strcmp(arg, options[i]) == 0) return 1;
    }
    return 0;
}

/* Returns a copy of argv with path option values resolved into the PAL fs
 * namespace (relative to the invoking shell, symlinks followed). Values the
 * target cannot map are left untouched so the App reports the PAL error. */
static const char *const *resolve_path_arguments(int argc, char **argv) {
    char buffer[4096];
    const char **args = calloc((size_t)argc + 1u, sizeof(*args));
    if (args == NULL) return NULL;
    for (int i = 0; i < argc; ++i) args[i] = argv[i];
    for (int i = 1; i + 1 < argc; ++i) {
        if (!is_path_option(argv[i])) continue;
        if (h2_h2loader_cli_target_resolve_path(argv[i + 1], buffer, sizeof(buffer))) {
            char *copy = malloc(strlen(buffer) + 1u);
            if (copy != NULL) args[i + 1] = strcpy(copy, buffer);
        }
        ++i;
    }
    return args;
}

int main(int argc, char **argv) {
    static const h2_command_io_vtable_t stdio_vtable = {
        .write = stdio_write,
        .flush = stdio_flush,
    };
    h2_command_io_api_t stdout_io = {.user = stdout, .vtable = &stdio_vtable};
    h2_command_io_api_t stderr_io = {.user = stderr, .vtable = &stdio_vtable};
    if (h2_h2loader_cli_target_start() != H2_PAL_OK) return 3;
    const char *const *args = resolve_path_arguments(argc, argv);
    if (args == NULL) return 3;
    const h2_pal_mem_api_t *mem = h2_h2loader_cli_target_mem();
    const h2_pal_ble_host_api_t *ble = h2_h2loader_cli_target_ble(mem);
    h2_runtime_t runtime = {
        .board = "host",
        .target = "cli",
        .chip = "native",
        .mem = mem,
        .ble_host = NULL,
        .log = h2_h2loader_cli_target_log(),
        .time = h2_h2loader_cli_target_time(),
        .task = h2_h2loader_cli_target_task(),
        .queue = h2_h2loader_cli_target_queue(),
        .sync = h2_h2loader_cli_target_sync(),
        .fs = h2_h2loader_cli_target_fs(),
        .net = h2_h2loader_cli_target_net(),
        .system_event = h2_h2loader_cli_target_system_event(),
    };
    h2_h2loader_cli_config_t config = {
        .argc = argc,
        .argv = args,
        .serial = h2_h2loader_cli_target_serial(),
        .stdout_io = &stdout_io,
        .stderr_io = &stderr_io,
        .is_cancelled = is_cancelled,
        .acquire_ble = ble != NULL ? acquire_ble : NULL,
        .ble_user = (void *)ble,
    };
    int result;
    (void)signal(SIGINT, handle_signal);
    (void)signal(SIGTERM, handle_signal);
    result = h2_h2loader_cli_main(&runtime, &config);
    if (started_ble != NULL) (void)h2_pal_ble_stop(started_ble);
    h2_h2loader_cli_target_stop();
    return cancelled ? 130 : result;
}
