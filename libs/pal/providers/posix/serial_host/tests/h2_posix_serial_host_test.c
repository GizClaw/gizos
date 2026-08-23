#if defined(__linux__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600
#endif

#include "h2_posix_serial_host_test.h"

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct read_fixture {
    const h2_pal_uart_io_stream_api_t *stream;
    h2_pal_result_t result;
} read_fixture_t;

static const h2_pal_uart_io_stream_config_t serial_config = {
    .baud_rate = 115200u,
    .data_bits = 8u,
    .stop_bits = 1u,
    .parity = H2_PAL_UART_PARITY_NONE,
    .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
};

static int create_pty(char **out_slave_path) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(master >= 0);
    assert(grantpt(master) == 0);
    assert(unlockpt(master) == 0);
    *out_slave_path = ptsname(master);
    assert(*out_slave_path != NULL);
    return master;
}

static void *bounded_read_thread(void *user) {
    read_fixture_t *fixture = (read_fixture_t *)user;
    uint8_t byte = 0u;
    size_t count = 0u;
    fixture->result = h2_pal_uart_io_stream_read(
        fixture->stream,
        &byte,
        sizeof(byte),
        &count,
        100u);
    assert(count == 0u);
    return NULL;
}

static void test_enumeration_snapshot(
    const h2_pal_serial_host_api_t *serial_host) {
    h2_pal_serial_host_snapshot_t *snapshot = NULL;
    size_t count = 0u;
    size_t index;
    assert(h2_pal_serial_host_scan(serial_host, &snapshot) == H2_PAL_OK);
    assert(snapshot != NULL);
    assert(h2_pal_serial_host_snapshot_count(
               serial_host,
               snapshot,
               &count) == H2_PAL_OK);
    for (index = 0u; index < count; ++index) {
        h2_pal_serial_host_port_info_t info;
        assert(h2_pal_serial_host_snapshot_get(
                   serial_host,
                   snapshot,
                   index,
                   &info) == H2_PAL_OK);
        assert(info.port_id[0] != '\0');
        assert(info.endpoint[0] != '\0');
        if (getenv("H2_DESKTOP_SERIAL_PRINT") != NULL) {
            printf(
                "%s vid=%s%04x pid=%s%04x serial=%s\n",
                info.endpoint,
                (info.valid_fields & H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID)
                    ? ""
                    : "unknown/",
                info.usb_vid,
                (info.valid_fields & H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID)
                    ? ""
                    : "unknown/",
                info.usb_pid,
                (info.valid_fields & H2_PAL_SERIAL_HOST_PORT_FIELD_USB_SERIAL)
                    ? info.usb_serial
                    : "unknown");
        }
#if defined(__APPLE__)
        assert(strncmp(info.endpoint, "/dev/cu.", strlen("/dev/cu.")) == 0);
#endif
    }
    assert(h2_pal_serial_host_snapshot_get(
               serial_host,
               snapshot,
               count,
               &(h2_pal_serial_host_port_info_t){0}) == H2_PAL_ERR_NOT_FOUND);
    assert(h2_pal_serial_host_snapshot_destroy(
               serial_host,
               &snapshot) == H2_PAL_OK);
    assert(snapshot == NULL);
    assert(h2_pal_serial_host_snapshot_destroy(
               serial_host,
               &snapshot) == H2_PAL_OK);
}

static void test_pty_io_and_lifecycle(
    const h2_pal_serial_host_api_t *serial_host) {
    char *slave_path = NULL;
    int master = create_pty(&slave_path);
    h2_pal_serial_host_session_t *session = NULL;
    h2_pal_serial_host_session_t *duplicate = NULL;
    const h2_pal_uart_io_stream_api_t *stream = NULL;
    uint8_t buffer[16] = {0};
    size_t transferred = 0u;

    assert(h2_pal_serial_host_open(
               serial_host,
               slave_path,
               &serial_config,
               &session) == H2_PAL_OK);
    assert(session != NULL);
    assert(h2_pal_serial_host_open(
               serial_host,
               slave_path,
               &serial_config,
               &duplicate) == H2_PAL_ERR_UNAVAILABLE);
    assert(duplicate == NULL);
    assert(h2_pal_serial_host_session_stream(
               serial_host,
               session,
               &stream) == H2_PAL_OK);
    assert(stream != NULL);

    assert(h2_pal_uart_io_stream_read(
               stream,
               buffer,
               sizeof(buffer),
               &transferred,
               10u) == H2_PAL_ERR_TIMEOUT);
    assert(transferred == 0u);
    assert(write(master, "host", 4u) == 4);
    assert(h2_pal_uart_io_stream_read(
               stream,
               buffer,
               sizeof(buffer),
               &transferred,
               100u) == H2_PAL_OK);
    assert(transferred == 4u);
    assert(memcmp(buffer, "host", 4u) == 0);

    assert(h2_pal_uart_io_stream_write(
               stream,
               "device",
               6u,
               &transferred,
               100u) == H2_PAL_OK);
    assert(transferred == 6u);
    assert(read(master, buffer, sizeof(buffer)) == 6);
    assert(memcmp(buffer, "device", 6u) == 0);
    assert(h2_pal_uart_io_stream_flush(stream) == H2_PAL_OK);

    {
        uint32_t lines = 0u;
        const h2_pal_result_t set_result =
            h2_pal_serial_host_set_control_lines(
                serial_host,
                session,
                H2_PAL_SERIAL_HOST_CONTROL_DTR |
                    H2_PAL_SERIAL_HOST_CONTROL_RTS,
                H2_PAL_SERIAL_HOST_CONTROL_DTR);
        const h2_pal_result_t get_result =
            h2_pal_serial_host_get_control_lines(
                serial_host,
                session,
                &lines);
        assert(set_result == H2_PAL_OK ||
               set_result == H2_PAL_ERR_UNSUPPORTED);
        assert(get_result == H2_PAL_OK ||
               get_result == H2_PAL_ERR_UNSUPPORTED);
    }

    close(master);
    assert(h2_pal_uart_io_stream_read(
               stream,
               buffer,
               sizeof(buffer),
               &transferred,
               100u) == H2_PAL_ERR_CLOSED);
    assert(h2_pal_serial_host_close(serial_host, &session) == H2_PAL_OK);
    assert(session == NULL);
    assert(h2_pal_serial_host_close(serial_host, &session) == H2_PAL_OK);
}

static void test_close_waits_for_bounded_io(
    const h2_pal_serial_host_api_t *serial_host) {
    char *slave_path = NULL;
    int master = create_pty(&slave_path);
    h2_pal_serial_host_session_t *session = NULL;
    const h2_pal_uart_io_stream_api_t *stream = NULL;
    read_fixture_t fixture;
    pthread_t thread;

    assert(h2_pal_serial_host_open(
               serial_host,
               slave_path,
               &serial_config,
               &session) == H2_PAL_OK);
    assert(h2_pal_serial_host_session_stream(
               serial_host,
               session,
               &stream) == H2_PAL_OK);
    fixture.stream = stream;
    fixture.result = H2_PAL_OK;
    assert(pthread_create(&thread, NULL, bounded_read_thread, &fixture) == 0);
    usleep(10u * 1000u);
    assert(h2_pal_serial_host_close(serial_host, &session) == H2_PAL_OK);
    assert(pthread_join(thread, NULL) == 0);
    assert(fixture.result == H2_PAL_ERR_TIMEOUT);
    close(master);
}

int h2_posix_serial_host_run_tests(
    const h2_pal_serial_host_api_t *serial_host) {
    assert(serial_host != NULL);
    assert(serial_host->vtable != NULL);
    test_enumeration_snapshot(serial_host);
    test_pty_io_and_lifecycle(serial_host);
    test_close_waits_for_bounded_io(serial_host);
    return 0;
}
