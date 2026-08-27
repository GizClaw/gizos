#include "h2_h2loader_cli_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct serial_policy_fixture {
    h2_pal_result_t scan_result;
    h2_pal_result_t count_result;
    h2_pal_result_t get_result;
    h2_pal_result_t destroy_result;
    h2_pal_serial_host_port_info_t ports[2];
    size_t port_count;
    unsigned scan_count;
    unsigned count_count;
    unsigned get_count;
    unsigned destroy_count;
    unsigned sequence;
    unsigned open_order;
    unsigned set_order;
    unsigned stream_order;
    unsigned close_order;
    unsigned open_count;
    unsigned set_count;
    unsigned stream_count;
    unsigned close_count;
    uint32_t line_mask;
    uint32_t asserted_lines;
} serial_policy_fixture_t;

static void *test_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static const h2_pal_mem_vtable_t test_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};

static const h2_pal_mem_api_t test_mem = {
    .vtable = &test_mem_vtable,
};

static const h2_pal_time_api_t test_time = {0};

static h2_pal_result_t policy_scan(
    void *user,
    h2_pal_serial_host_snapshot_t **out_snapshot) {
    serial_policy_fixture_t *fixture = user;
    ++fixture->scan_count;
    if (fixture->scan_result != H2_PAL_OK) {
        return fixture->scan_result;
    }
    *out_snapshot = (h2_pal_serial_host_snapshot_t *)fixture;
    return H2_PAL_OK;
}

static h2_pal_result_t policy_snapshot_count(
    void *user,
    const h2_pal_serial_host_snapshot_t *snapshot,
    size_t *out_count) {
    serial_policy_fixture_t *fixture = user;
    assert(snapshot == (const h2_pal_serial_host_snapshot_t *)fixture);
    ++fixture->count_count;
    if (fixture->count_result == H2_PAL_OK) {
        *out_count = fixture->port_count;
    }
    return fixture->count_result;
}

static h2_pal_result_t policy_snapshot_get(
    void *user,
    const h2_pal_serial_host_snapshot_t *snapshot,
    size_t index,
    h2_pal_serial_host_port_info_t *out_info) {
    serial_policy_fixture_t *fixture = user;
    assert(snapshot == (const h2_pal_serial_host_snapshot_t *)fixture);
    assert(index < fixture->port_count);
    ++fixture->get_count;
    if (fixture->get_result == H2_PAL_OK) {
        *out_info = fixture->ports[index];
    }
    return fixture->get_result;
}

static h2_pal_result_t policy_snapshot_destroy(
    void *user,
    h2_pal_serial_host_snapshot_t **inout_snapshot) {
    serial_policy_fixture_t *fixture = user;
    assert(*inout_snapshot == (h2_pal_serial_host_snapshot_t *)fixture);
    ++fixture->destroy_count;
    if (fixture->destroy_result == H2_PAL_OK) {
        *inout_snapshot = NULL;
    }
    return fixture->destroy_result;
}

static h2_pal_result_t policy_open(
    void *user,
    const char *port_id,
    const h2_pal_uart_io_stream_config_t *config,
    h2_pal_serial_host_session_t **out_session) {
    serial_policy_fixture_t *fixture = user;
    assert(strcmp(port_id, "port-a") == 0);
    assert(config->baud_rate == H2_H2LOADER_HOST_RELIABLE_SERIAL_BAUD);
    fixture->open_order = ++fixture->sequence;
    ++fixture->open_count;
    *out_session = (h2_pal_serial_host_session_t *)fixture;
    return H2_PAL_OK;
}

static h2_pal_result_t policy_set_control_lines(
    void *user,
    h2_pal_serial_host_session_t *session,
    uint32_t line_mask,
    uint32_t asserted_lines) {
    serial_policy_fixture_t *fixture = user;
    assert(session == (h2_pal_serial_host_session_t *)fixture);
    fixture->set_order = ++fixture->sequence;
    ++fixture->set_count;
    fixture->line_mask = line_mask;
    fixture->asserted_lines = asserted_lines;
    return H2_PAL_OK;
}

static h2_pal_result_t policy_session_stream(
    void *user,
    h2_pal_serial_host_session_t *session,
    const h2_pal_uart_io_stream_api_t **out_stream) {
    serial_policy_fixture_t *fixture = user;
    assert(session == (h2_pal_serial_host_session_t *)fixture);
    fixture->stream_order = ++fixture->sequence;
    ++fixture->stream_count;
    *out_stream = NULL;
    return H2_PAL_ERR_IO;
}

static h2_pal_result_t policy_close(
    void *user,
    h2_pal_serial_host_session_t **inout_session) {
    serial_policy_fixture_t *fixture = user;
    assert(*inout_session == (h2_pal_serial_host_session_t *)fixture);
    fixture->close_order = ++fixture->sequence;
    ++fixture->close_count;
    *inout_session = NULL;
    return H2_PAL_OK;
}

static const h2_pal_serial_host_vtable_t policy_serial_vtable = {
    .scan = policy_scan,
    .snapshot_count = policy_snapshot_count,
    .snapshot_get = policy_snapshot_get,
    .snapshot_destroy = policy_snapshot_destroy,
    .open = policy_open,
    .session_stream = policy_session_stream,
    .set_control_lines = policy_set_control_lines,
    .close = policy_close,
};

static h2_h2loader_cli_transport_t make_transport(
    serial_policy_fixture_t *fixture,
    h2_h2loader_cli_context_t *context,
    h2_runtime_t *runtime,
    h2_h2loader_cli_config_t *config,
    h2_pal_serial_host_api_t *serial,
    h2_h2loader_cli_options_t *options) {
    *serial = (h2_pal_serial_host_api_t){
        .user = fixture,
        .vtable = &policy_serial_vtable,
    };
    *config = (h2_h2loader_cli_config_t){
        .serial = serial,
    };
    *context = (h2_h2loader_cli_context_t){
        .runtime = runtime,
        .config = config,
    };
    runtime->mem = &test_mem;
    runtime->time = &test_time;
    *options = (h2_h2loader_cli_options_t){
        .port = "port-a",
        .transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL,
    };
    h2_h2loader_cli_transport_t transport;
    h2_h2loader_cli_transport_init(
        &transport, context, options, 1000u);
    return transport;
}

static void set_port(
    h2_pal_serial_host_port_info_t *port,
    const char *port_id,
    uint32_t valid_fields,
    uint16_t usb_vid,
    uint16_t usb_pid) {
    memset(port, 0, sizeof(*port));
    strcpy(port->port_id, port_id);
    strcpy(port->endpoint, port_id);
    port->valid_fields = valid_fields;
    port->usb_vid = usb_vid;
    port->usb_pid = usb_pid;
}

static void assert_deassert_connection_attempts(
    const serial_policy_fixture_t *fixture,
    unsigned attempts) {
    assert(fixture->open_count == attempts);
    assert(fixture->set_count == attempts);
    assert(fixture->stream_count == attempts);
    assert(fixture->close_count == attempts);
    assert(fixture->open_order < fixture->set_order);
    assert(fixture->set_order < fixture->stream_order);
    assert(fixture->stream_order < fixture->close_order);
    assert(fixture->line_mask ==
        (H2_PAL_SERIAL_HOST_CONTROL_DTR |
         H2_PAL_SERIAL_HOST_CONTROL_RTS));
    assert(fixture->asserted_lines == 0u);
}

static void test_direct_connect_and_reconnect_cache_policy(void) {
    serial_policy_fixture_t fixture = {
        .scan_result = H2_PAL_OK,
        .count_result = H2_PAL_OK,
        .get_result = H2_PAL_OK,
        .destroy_result = H2_PAL_OK,
        .port_count = 1u,
    };
    h2_h2loader_cli_context_t context;
    h2_runtime_t runtime = {0};
    h2_h2loader_cli_config_t config;
    h2_pal_serial_host_api_t serial;
    h2_h2loader_cli_options_t options;
    h2_h2loader_host_status_t status;
    set_port(&fixture.ports[0], "port-a",
        H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID |
            H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID,
        0x1a86u, 0x7523u);
    h2_h2loader_cli_transport_t transport = make_transport(
        &fixture, &context, &runtime, &config, &serial, &options);

    assert(h2_h2loader_cli_transport_connect(&transport, &status) ==
        H2_PAL_ERR_IO);
    assert(transport.serial_control_line_mask ==
        (H2_PAL_SERIAL_HOST_CONTROL_DTR |
         H2_PAL_SERIAL_HOST_CONTROL_RTS));
    assert(transport.serial_asserted_control_lines == 0u);
    assert(fixture.scan_count == 1u);
    assert(fixture.destroy_count == 1u);
    assert_deassert_connection_attempts(&fixture, 1u);

    assert(h2_h2loader_cli_transport_connect(&transport, &status) ==
        H2_PAL_ERR_IO);
    assert(fixture.scan_count == 1u);
    assert(fixture.destroy_count == 1u);
    assert_deassert_connection_attempts(&fixture, 2u);
}

static void test_scan_probe_uses_frozen_candidate(void) {
    serial_policy_fixture_t fixture = {0};
    h2_h2loader_cli_context_t context;
    h2_runtime_t runtime = {0};
    h2_h2loader_cli_config_t config;
    h2_pal_serial_host_api_t serial;
    h2_h2loader_cli_options_t options;
    h2_h2loader_host_status_t status;
    h2_h2loader_host_candidate_t candidate = {
        .transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL,
        .serial_valid_fields =
            H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID |
            H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID,
        .usb_vid = 0x1a86u,
        .usb_pid = 0x7523u,
    };
    (void)make_transport(
        &fixture, &context, &runtime, &config, &serial, &options);
    strcpy(candidate.port_id, "port-a");

    assert(h2_h2loader_cli_scan_probe_serial(
               &context, &candidate, 1000u, &status) == H2_PAL_ERR_IO);
    assert(fixture.scan_count == 0u);
    assert(fixture.destroy_count == 0u);
    assert_deassert_connection_attempts(&fixture, 1u);
}

static void test_frozen_candidate_policy(void) {
    serial_policy_fixture_t fixture = {0};
    h2_h2loader_cli_context_t context;
    h2_runtime_t runtime = {0};
    h2_h2loader_cli_config_t config;
    h2_pal_serial_host_api_t serial;
    h2_h2loader_cli_options_t options;
    h2_h2loader_cli_transport_t transport = make_transport(
        &fixture, &context, &runtime, &config, &serial, &options);
    h2_h2loader_host_candidate_t candidate = {
        .transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL,
        .serial_valid_fields =
            H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID |
            H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID,
        .usb_vid = 0x303au,
        .usb_pid = 0x1001u,
    };
    strcpy(candidate.port_id, "port-a");

    assert(h2_h2loader_cli_transport_set_serial_candidate(
               &transport, &candidate) == H2_PAL_OK);
    assert(h2_h2loader_cli_transport_prepare_serial_policy(&transport) ==
        H2_PAL_OK);
#if defined(__APPLE__) && defined(__MACH__)
    assert(transport.serial_control_line_mask == 0u);
#else
    assert(transport.serial_control_line_mask ==
        (H2_PAL_SERIAL_HOST_CONTROL_DTR |
         H2_PAL_SERIAL_HOST_CONTROL_RTS));
#endif
    assert(fixture.scan_count == 0u);

    transport.serial_policy_valid = 0u;
    transport.serial_candidate.serial_valid_fields =
        H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID;
    assert(h2_h2loader_cli_transport_prepare_serial_policy(&transport) ==
        H2_PAL_OK);
    assert(transport.serial_control_line_mask ==
        (H2_PAL_SERIAL_HOST_CONTROL_DTR |
         H2_PAL_SERIAL_HOST_CONTROL_RTS));

    strcpy(candidate.port_id, "other-port");
    assert(h2_h2loader_cli_transport_set_serial_candidate(
               &transport, &candidate) == H2_PAL_ERR_INVALID_ARG);
}

static void test_direct_metadata_fallback_and_cleanup(void) {
    serial_policy_fixture_t fixture = {
        .scan_result = H2_PAL_ERR_UNAVAILABLE,
        .count_result = H2_PAL_OK,
        .get_result = H2_PAL_OK,
        .destroy_result = H2_PAL_OK,
    };
    h2_h2loader_cli_context_t context;
    h2_runtime_t runtime = {0};
    h2_h2loader_cli_config_t config;
    h2_pal_serial_host_api_t serial;
    h2_h2loader_cli_options_t options;
    h2_h2loader_cli_transport_t transport = make_transport(
        &fixture, &context, &runtime, &config, &serial, &options);

    assert(h2_h2loader_cli_transport_prepare_serial_policy(&transport) ==
        H2_PAL_OK);
    assert(transport.serial_control_line_mask ==
        (H2_PAL_SERIAL_HOST_CONTROL_DTR |
         H2_PAL_SERIAL_HOST_CONTROL_RTS));
    assert(fixture.destroy_count == 0u);

    fixture.scan_result = H2_PAL_OK;
    fixture.destroy_result = H2_PAL_OK;
    fixture.port_count = 1u;
    set_port(&fixture.ports[0], "other-port", 0u, 0u, 0u);
    transport.serial_policy_valid = 0u;
    transport.serial_candidate_valid = 0u;
    assert(h2_h2loader_cli_transport_prepare_serial_policy(&transport) ==
        H2_PAL_OK);
    assert(transport.serial_control_line_mask ==
        (H2_PAL_SERIAL_HOST_CONTROL_DTR |
         H2_PAL_SERIAL_HOST_CONTROL_RTS));
    assert(fixture.destroy_count == 1u);

    fixture.destroy_result = H2_PAL_ERR_IO;
    transport.serial_policy_valid = 0u;
    transport.serial_candidate_valid = 0u;
    assert(h2_h2loader_cli_transport_prepare_serial_policy(&transport) ==
        H2_PAL_ERR_IO);
    assert(transport.serial_policy_valid == 0u);
    assert(fixture.destroy_count == 2u);
}

int main(void) {
    test_direct_connect_and_reconnect_cache_policy();
    test_scan_probe_uses_frozen_candidate();
    test_frozen_candidate_policy();
    test_direct_metadata_fallback_and_cleanup();
    puts("h2loader cli transport tests passed");
    return 0;
}
