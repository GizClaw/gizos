#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_serial_host_scan(
    void *user,
    h2_pal_serial_host_snapshot_t **out_snapshot) {
    (void)user;
    (void)out_snapshot;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_serial_host_snapshot_count(
    void *user,
    const h2_pal_serial_host_snapshot_t *snapshot,
    size_t *out_count) {
    (void)user;
    (void)snapshot;
    (void)out_count;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_serial_host_snapshot_get(
    void *user,
    const h2_pal_serial_host_snapshot_t *snapshot,
    size_t index,
    h2_pal_serial_host_port_info_t *out_info) {
    (void)user;
    (void)snapshot;
    (void)index;
    (void)out_info;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_serial_host_snapshot_destroy(
    void *user,
    h2_pal_serial_host_snapshot_t **inout_snapshot) {
    (void)user;
    (void)inout_snapshot;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_serial_host_open(
    void *user,
    const char *port_id,
    const h2_pal_uart_io_stream_config_t *config,
    h2_pal_serial_host_session_t **out_session) {
    (void)user;
    (void)port_id;
    (void)config;
    (void)out_session;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_serial_host_session_stream(
    void *user,
    h2_pal_serial_host_session_t *session,
    const h2_pal_uart_io_stream_api_t **out_stream) {
    (void)user;
    (void)session;
    (void)out_stream;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_serial_host_set_control_lines(
    void *user,
    h2_pal_serial_host_session_t *session,
    uint32_t line_mask,
    uint32_t asserted_lines) {
    (void)user;
    (void)session;
    (void)line_mask;
    (void)asserted_lines;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_serial_host_get_control_lines(
    void *user,
    h2_pal_serial_host_session_t *session,
    uint32_t *out_asserted_lines) {
    (void)user;
    (void)session;
    (void)out_asserted_lines;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_serial_host_close(
    void *user,
    h2_pal_serial_host_session_t **inout_session) {
    (void)user;
    (void)inout_session;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_serial_host_vtable_t unsupported_serial_host_vtable = {
    .scan = unsupported_serial_host_scan,
    .snapshot_count = unsupported_serial_host_snapshot_count,
    .snapshot_get = unsupported_serial_host_snapshot_get,
    .snapshot_destroy = unsupported_serial_host_snapshot_destroy,
    .open = unsupported_serial_host_open,
    .session_stream = unsupported_serial_host_session_stream,
    .set_control_lines = unsupported_serial_host_set_control_lines,
    .get_control_lines = unsupported_serial_host_get_control_lines,
    .close = unsupported_serial_host_close,
};

static const h2_pal_serial_host_api_t unsupported_serial_host_api = {
    .user = NULL,
    .vtable = &unsupported_serial_host_vtable,
};

const h2_pal_serial_host_api_t *h2_pal_unsupported_serial_host_api(void) {
    return &unsupported_serial_host_api;
}
