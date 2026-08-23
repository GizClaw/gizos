#ifndef H2_H2LOADER_CLI_INTERNAL_H
#define H2_H2LOADER_CLI_INTERNAL_H

#include "h2_h2loader_cli_app.h"
#include "h2_h2loader_host_package.h"

#define H2_H2LOADER_CLI_MAX_DATA_ENTRIES 128u

typedef struct h2_h2loader_cli_options {
    const char *port;
    const char *ready_marker;
    uint32_t wait_timeout_ms;
    uint32_t read_timeout_ms;
    uint32_t post_delay_ms;
    int command_index;
    int no_ble;
} h2_h2loader_cli_options_t;

typedef struct h2_h2loader_cli_server_options {
    const char *url;
    const char *file_path;
    const char *bind_host;
    const char *url_host;
    const char *url_path;
    const char *ssid;
    const char *password;
    const char *expected_sha256;
    uint64_t expected_bytes;
    uint32_t download_timeout_ms;
    uint16_t port;
} h2_h2loader_cli_server_options_t;

typedef struct h2_h2loader_cli_server_transport_vtable {
    h2_pal_result_t (*connect)(
        void *user,
        const h2_h2loader_host_serial_connection_config_t *config);
    h2_pal_result_t (*read_status)(
        void *user,
        h2_h2loader_host_status_t *out_status);
    h2_pal_result_t (*execute)(
        void *user,
        const h2_h2loader_host_command_request_t *request,
        h2_h2loader_host_command_result_t *out_result);
    h2_pal_result_t (*disconnect)(void *user);
} h2_h2loader_cli_server_transport_vtable_t;

typedef struct h2_h2loader_cli_server_transport_api {
    void *user;
    const h2_h2loader_cli_server_transport_vtable_t *vtable;
} h2_h2loader_cli_server_transport_api_t;

typedef struct h2_h2loader_cli_context {
    h2_runtime_t *runtime;
    const h2_h2loader_cli_config_t *config;
    int ble_disabled;
} h2_h2loader_cli_context_t;

const h2_pal_ble_host_api_t *h2_h2loader_cli_acquire_ble(
    h2_h2loader_cli_context_t *context);

typedef struct h2_h2loader_cli_send_progress {
    h2_h2loader_cli_context_t *context;
    uint64_t start_ms;
    uint64_t last_report_ms;
    uint32_t last_percent;
    uint8_t reported;
} h2_h2loader_cli_send_progress_t;

h2_pal_result_t h2_h2loader_cli_output(
    h2_h2loader_cli_context_t *context,
    h2_h2loader_cli_stream_t stream,
    const char *format,
    ...);

h2_pal_result_t h2_h2loader_cli_output_bytes(
    h2_h2loader_cli_context_t *context,
    h2_h2loader_cli_stream_t stream,
    const uint8_t *data,
    size_t len);

h2_pal_result_t h2_h2loader_cli_output_json_string(
    h2_h2loader_cli_context_t *context,
    h2_h2loader_cli_stream_t stream,
    const char *value);

void h2_h2loader_cli_send_progress(
    void *user,
    uint64_t acknowledged_bytes,
    uint64_t total_bytes);

int h2_h2loader_cli_package_command(
    h2_h2loader_cli_context_t *context,
    int argc,
    const char *const *argv,
    int golden);

int h2_h2loader_cli_server_options_parse(
    int argc,
    const char *const *argv,
    h2_h2loader_cli_server_options_t *out);

int h2_h2loader_cli_server_command(
    h2_h2loader_cli_context_t *context,
    const h2_h2loader_cli_options_t *options,
    int argc,
    const char *const *argv);

int h2_h2loader_cli_server_command_with_transport(
    h2_h2loader_cli_context_t *context,
    const h2_h2loader_cli_options_t *options,
    int argc,
    const char *const *argv,
    const h2_h2loader_cli_server_transport_api_t *transport);

h2_pal_result_t h2_h2loader_cli_find_ble_peer(
    h2_h2loader_cli_context_t *context,
    uint32_t timeout_ms,
    h2_pal_ble_addr_t *out_address,
    int *out_rssi);

int h2_h2loader_cli_bleikcp_speed_command(
    h2_h2loader_cli_context_t *context,
    int argc,
    const char *const *argv);

#endif
