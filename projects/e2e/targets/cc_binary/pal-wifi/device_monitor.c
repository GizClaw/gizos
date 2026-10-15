#include "h2_darwin_platform.h"
#include "h2_desktop_platform.h"
#include "h2_h2loader_host.h"
#include <stdio.h>
#include <string.h>

typedef struct monitor {
    const h2_pal_time_api_t *time;
    uint64_t start;
    int complete;
    char line[1024];
    size_t length;
} monitor_t;
static int cancelled(void *user) {
    monitor_t *m = user;
    uint64_t now = 0;
    if (h2_pal_time_get_monotonic_ms(m->time, &now))
        return 1;
    return m->complete || now - m->start > 900000;
}
static int log_bytes(void *user, const uint8_t *data, size_t length) {
    monitor_t *m = user;
    if (fwrite(data, 1, length, stdout) != length || fflush(stdout))
        return H2_PAL_ERR_IO;
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == '\n' || data[i] == '\r') {
            m->line[m->length] = 0;
            if (strstr(m->line, "H2_WIFI_READY "))
                m->complete = 1;
            m->length = 0;
        } else if (m->length + 1 < sizeof(m->line))
            m->line[m->length++] = (char)data[i];
        else
            m->length = 0;
    }
    return H2_PAL_OK;
}
int main(int argc, char **argv) {
    if (argc != 5) {
        fputs("device_monitor PORT UID VERSION upgrade|app|attach\n", stderr);
        return 2;
    }
    monitor_t m = {.time = h2_desktop_platform_time_api()};
    if (h2_pal_time_get_monotonic_ms(m.time, &m.start))
        return 3;
    h2_h2loader_host_serial_connection_config_t config = {
        .serial = h2_darwin_serial_host_api(),
        .time = m.time,
        .allocator = h2_desktop_platform_default_allocator(),
        .port_id = argv[1],
        .baud_rate = 460800,
        .handshake_timeout_ms = 15000,
        .command_timeout_ms = 15000,
        .on_log = log_bytes,
        .log_user = &m,
        .preserve_control_lines = 1};
    h2_h2loader_host_serial_connection_t *connection = NULL;
    h2_h2loader_host_status_t status = {0};
    int rc = h2_h2loader_host_serial_connect(&config, &connection);
    if (!rc)
        rc = h2_h2loader_host_serial_read_status(connection, &status);
    if (!rc && strcmp(status.device_uid, argv[2]))
        rc = H2_PAL_ERR_INVALID_STATE;
    if (!rc && strcmp(argv[4], "attach")) {
        h2_h2loader_host_command_request_t request = {
            .status = &status,
            .command = !strcmp(argv[4], "upgrade") ? H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE
                                                   : H2_H2LOADER_HOST_COMMAND_REBOOT_APP};
        h2_h2loader_host_command_result_t result = {0};
        rc = h2_h2loader_host_serial_execute_command(connection, &request, &result);
        if (!rc && result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK)
            rc = H2_PAL_ERR_IO;
        (void)h2_h2loader_host_serial_disconnect(&connection);
        /* Image remains unconfirmed until the App's terminal qualification.
         * Reconnect checks UID/version/role, never prematurely requires an
         * empty Stage. The receipt checker requires final confirmation. */
        for (unsigned i = 0; !rc && i < 60; ++i) {
            (void)h2_pal_time_sleep_ms(m.time, 500);
            int next = h2_h2loader_host_serial_connect(&config, &connection);
            if (!next)
                next = h2_h2loader_host_serial_read_status(connection, &status);
            if (!next && strcmp(status.device_uid, argv[2])) {
                rc = H2_PAL_ERR_INVALID_STATE;
                break;
            }
            if (!next && !strcmp(status.active_version, argv[3]) && status.running_partition == 2)
                break;
            (void)h2_h2loader_host_serial_disconnect(&connection);
            if (i == 59)
                rc = H2_PAL_ERR_TIMEOUT;
        }
    }
    if (!rc)
        rc = h2_h2loader_host_serial_monitor_logs(connection, cancelled, &m);
    (void)h2_h2loader_host_serial_disconnect(&connection);
    fprintf(stderr, "H2_WIFI_MONITOR complete=%d rc=%d\n", m.complete, rc);
    return m.complete && (rc == H2_PAL_OK || rc == H2_PAL_EXIT) ? 0 : 1;
}
