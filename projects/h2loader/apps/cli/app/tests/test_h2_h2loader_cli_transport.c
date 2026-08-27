#include "h2_h2loader_cli_internal.h"

#include <assert.h>
#include <stdio.h>
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
} serial_policy_fixture_t;

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

static const h2_pal_serial_host_vtable_t policy_serial_vtable = {
    .scan = policy_scan,
    .snapshot_count = policy_snapshot_count,
    .snapshot_get = policy_snapshot_get,
    .snapshot_destroy = policy_snapshot_destroy,
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

static void test_direct_policy_is_cached(void) {
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
    set_port(&fixture.ports[0], "port-a",
        H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID |
            H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID,
        0x1a86u, 0x7523u);
    h2_h2loader_cli_transport_t transport = make_transport(
        &fixture, &context, &runtime, &config, &serial, &options);

    assert(h2_h2loader_cli_transport_prepare_serial_policy(&transport) ==
        H2_PAL_OK);
    assert(transport.serial_control_line_mask ==
        (H2_PAL_SERIAL_HOST_CONTROL_DTR |
         H2_PAL_SERIAL_HOST_CONTROL_RTS));
    assert(transport.serial_asserted_control_lines == 0u);
    assert(fixture.scan_count == 1u);
    assert(fixture.destroy_count == 1u);
    assert(h2_h2loader_cli_transport_prepare_serial_policy(&transport) ==
        H2_PAL_OK);
    assert(fixture.scan_count == 1u);
    assert(fixture.destroy_count == 1u);
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
    test_direct_policy_is_cached();
    test_frozen_candidate_policy();
    test_direct_metadata_fallback_and_cleanup();
    puts("h2loader cli transport tests passed");
    return 0;
}
