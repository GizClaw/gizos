#include "h2_windows_serial_host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

typedef struct serial_test_state {
    COMMTIMEOUTS original_timeouts;
    COMMTIMEOUTS applied_timeouts;
    DCB original_dcb;
    DWORD read_count;
    DWORD write_count;
    DWORD read_error;
    DWORD write_error;
    DWORD queue_depth;
    DWORD escape_error;
    DWORD sleep_advance_ms;
    DWORD escape_operations[8];
    size_t escape_count;
    ULONGLONG tick_ms;
    int set_timeouts_calls;
    int set_state_calls;
    int close_calls;
} serial_test_state_t;

static serial_test_state_t test_state;
static const HANDLE test_handle = (HANDLE)(uintptr_t)0x1234u;

DWORD WINAPI h2_test_QueryDosDeviceW(LPCWSTR device_name, LPWSTR target_path,
                                     DWORD capacity) {
    assert(device_name == NULL);
    static const wchar_t devices[] =
        L"COM10\0LPT1\0com2\0COM1\0COMX\0\0";
    const DWORD required = (DWORD)(sizeof(devices) / sizeof(devices[0]));
    if (capacity < required) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0u;
    }
    memcpy(target_path, devices, sizeof(devices));
    return required;
}

HANDLE WINAPI h2_test_CreateFileW(LPCWSTR path, DWORD access, DWORD share_mode,
                                  LPSECURITY_ATTRIBUTES security,
                                  DWORD creation, DWORD flags,
                                  HANDLE template_file) {
    assert(wcscmp(path, L"\\\\.\\COM9") == 0);
    assert(access == (GENERIC_READ | GENERIC_WRITE));
    assert(share_mode == 0u);
    assert(security == NULL);
    assert(creation == OPEN_EXISTING);
    assert(flags == FILE_ATTRIBUTE_NORMAL);
    assert(template_file == NULL);
    return test_handle;
}

BOOL WINAPI h2_test_GetCommState(HANDLE handle, LPDCB dcb) {
    assert(handle == test_handle);
    *dcb = test_state.original_dcb;
    return TRUE;
}

BOOL WINAPI h2_test_GetCommTimeouts(HANDLE handle, LPCOMMTIMEOUTS timeouts) {
    assert(handle == test_handle);
    *timeouts = test_state.original_timeouts;
    return TRUE;
}

BOOL WINAPI h2_test_SetCommState(HANDLE handle, LPDCB dcb) {
    assert(handle == test_handle);
    assert(dcb != NULL);
    ++test_state.set_state_calls;
    return TRUE;
}

BOOL WINAPI h2_test_SetupComm(HANDLE handle, DWORD input_size,
                              DWORD output_size) {
    assert(handle == test_handle);
    assert(input_size != 0u);
    assert(output_size != 0u);
    return TRUE;
}

BOOL WINAPI h2_test_SetCommTimeouts(HANDLE handle,
                                    LPCOMMTIMEOUTS timeouts) {
    assert(handle == test_handle);
    test_state.applied_timeouts = *timeouts;
    ++test_state.set_timeouts_calls;
    return TRUE;
}

BOOL WINAPI h2_test_ReadFile(HANDLE handle, LPVOID buffer, DWORD length,
                             LPDWORD out_count, LPOVERLAPPED overlapped) {
    assert(handle == test_handle);
    assert(buffer != NULL);
    assert(length != 0u);
    assert(overlapped == NULL);
    if (test_state.read_error != ERROR_SUCCESS) {
        SetLastError(test_state.read_error);
        return FALSE;
    }
    *out_count = test_state.read_count;
    return TRUE;
}

BOOL WINAPI h2_test_WriteFile(HANDLE handle, LPCVOID buffer, DWORD length,
                              LPDWORD out_count, LPOVERLAPPED overlapped) {
    assert(handle == test_handle);
    assert(buffer != NULL);
    assert(length != 0u);
    assert(overlapped == NULL);
    if (test_state.write_error != ERROR_SUCCESS) {
        SetLastError(test_state.write_error);
        return FALSE;
    }
    *out_count = test_state.write_count;
    return TRUE;
}

BOOL WINAPI h2_test_ClearCommError(HANDLE handle, LPDWORD errors,
                                   LPCOMSTAT status) {
    assert(handle == test_handle);
    *errors = 0u;
    memset(status, 0, sizeof(*status));
    status->cbOutQue = test_state.queue_depth;
    return TRUE;
}

ULONGLONG WINAPI h2_test_GetTickCount64(void) { return test_state.tick_ms; }

VOID WINAPI h2_test_Sleep(DWORD duration_ms) {
    test_state.tick_ms += test_state.sleep_advance_ms == 0u
                              ? duration_ms
                              : test_state.sleep_advance_ms;
    if (test_state.sleep_advance_ms == 0u && test_state.queue_depth != 0u) {
        test_state.queue_depth = 0u;
    }
}

BOOL WINAPI h2_test_EscapeCommFunction(HANDLE handle, DWORD operation) {
    assert(handle == test_handle);
    if (test_state.escape_error != ERROR_SUCCESS) {
        SetLastError(test_state.escape_error);
        return FALSE;
    }
    assert(test_state.escape_count <
           sizeof(test_state.escape_operations) /
               sizeof(test_state.escape_operations[0]));
    test_state.escape_operations[test_state.escape_count++] = operation;
    return TRUE;
}

BOOL WINAPI h2_test_CloseHandle(HANDLE handle) {
    assert(handle == test_handle);
    ++test_state.close_calls;
    return TRUE;
}

static h2_pal_serial_host_session_t *open_test_session(
    const h2_pal_serial_host_api_t *api) {
    const h2_pal_uart_io_stream_config_t config = {
        .baud_rate = 230400u,
        .data_bits = 8u,
        .stop_bits = 1u,
        .parity = H2_PAL_UART_PARITY_NONE,
        .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
    };
    h2_pal_serial_host_session_t *session = NULL;
    assert(h2_pal_serial_host_open(api, "COM9", &config, &session) ==
           H2_PAL_OK);
    assert(session != NULL);
    return session;
}

static void test_ordered_com_discovery(const h2_pal_serial_host_api_t *api) {
    h2_pal_serial_host_snapshot_t *snapshot = NULL;
    assert(h2_pal_serial_host_scan(api, &snapshot) == H2_PAL_OK);
    size_t count = 0u;
    assert(h2_pal_serial_host_snapshot_count(api, snapshot, &count) ==
           H2_PAL_OK);
    assert(count == 3u);
    static const char *expected[] = {"COM1", "COM2", "COM10"};
    for (size_t index = 0u; index < count; ++index) {
        h2_pal_serial_host_port_info_t info;
        assert(h2_pal_serial_host_snapshot_get(api, snapshot, index, &info) ==
               H2_PAL_OK);
        assert(strcmp(info.port_id, expected[index]) == 0);
        assert(strcmp(info.endpoint, expected[index]) == 0);
        assert(info.capabilities ==
               (H2_PAL_SERIAL_HOST_CAP_DTR | H2_PAL_SERIAL_HOST_CAP_RTS));
    }
    assert(h2_pal_serial_host_snapshot_destroy(api, &snapshot) == H2_PAL_OK);
    assert(snapshot == NULL);
}

static void test_partial_io_bounded_timeouts_flush_controls_disconnect_and_close(
    const h2_pal_serial_host_api_t *api) {
    h2_pal_serial_host_session_t *session = open_test_session(api);
    const h2_pal_uart_io_stream_api_t *stream = NULL;
    assert(h2_pal_serial_host_session_stream(api, session, &stream) ==
           H2_PAL_OK);

    const char payload[] = "abcd";
    size_t transferred = 0u;
    test_state.write_count = 2u;
    assert(h2_pal_uart_io_stream_write(stream, payload, sizeof(payload),
                                       &transferred, 0u) == H2_PAL_OK);
    assert(transferred == 2u);
    assert(test_state.applied_timeouts.WriteTotalTimeoutMultiplier == 0u);
    assert(test_state.applied_timeouts.WriteTotalTimeoutConstant == 1u);

    test_state.write_count = 0u;
    assert(h2_pal_uart_io_stream_write(stream, payload, sizeof(payload),
                                       &transferred, 25u) ==
           H2_PAL_ERR_TIMEOUT);
    assert(test_state.applied_timeouts.WriteTotalTimeoutMultiplier == 0u);
    assert(test_state.applied_timeouts.WriteTotalTimeoutConstant == 25u);
    test_state.write_error = ERROR_DEVICE_NOT_CONNECTED;
    assert(h2_pal_uart_io_stream_write(stream, payload, sizeof(payload),
                                       &transferred, 25u) ==
           H2_PAL_ERR_CLOSED);
    test_state.write_error = ERROR_SUCCESS;

    char buffer[4];
    test_state.read_count = 3u;
    assert(h2_pal_uart_io_stream_read(stream, buffer, sizeof(buffer),
                                      &transferred, 7u) == H2_PAL_OK);
    assert(transferred == 3u);
    test_state.read_count = 0u;
    assert(h2_pal_uart_io_stream_read(stream, buffer, sizeof(buffer),
                                      &transferred, 0u) ==
           H2_PAL_ERR_TIMEOUT);
    test_state.read_error = ERROR_OPERATION_ABORTED;
    assert(h2_pal_uart_io_stream_read(stream, buffer, sizeof(buffer),
                                      &transferred, 7u) ==
           H2_PAL_ERR_CLOSED);
    test_state.read_error = ERROR_SUCCESS;

    test_state.queue_depth = 8u;
    test_state.tick_ms = 100u;
    assert(h2_pal_uart_io_stream_flush(stream) == H2_PAL_OK);
    test_state.queue_depth = 8u;
    test_state.sleep_advance_ms = 10000u;
    assert(h2_pal_uart_io_stream_flush(stream) == H2_PAL_ERR_TIMEOUT);
    test_state.queue_depth = 0u;
    test_state.sleep_advance_ms = 0u;

    assert(h2_pal_serial_host_set_control_lines(
               api, session,
               H2_PAL_SERIAL_HOST_CONTROL_DTR |
                   H2_PAL_SERIAL_HOST_CONTROL_RTS,
               H2_PAL_SERIAL_HOST_CONTROL_DTR) == H2_PAL_OK);
    assert(test_state.escape_count == 2u);
    assert(test_state.escape_operations[0] == SETDTR);
    assert(test_state.escape_operations[1] == CLRRTS);
    uint32_t asserted = 0u;
    assert(h2_pal_serial_host_get_control_lines(api, session, &asserted) ==
           H2_PAL_OK);
    assert(asserted == H2_PAL_SERIAL_HOST_CONTROL_DTR);
    test_state.escape_error = ERROR_DEVICE_NOT_CONNECTED;
    assert(h2_pal_serial_host_set_control_lines(
               api, session, H2_PAL_SERIAL_HOST_CONTROL_DTR, 0u) ==
           H2_PAL_ERR_CLOSED);
    test_state.escape_error = ERROR_SUCCESS;

    int prior_timeout_calls = test_state.set_timeouts_calls;
    int prior_state_calls = test_state.set_state_calls;
    assert(h2_pal_serial_host_close(api, &session) == H2_PAL_OK);
    assert(session == NULL);
    assert(test_state.set_timeouts_calls == prior_timeout_calls + 1);
    assert(test_state.set_state_calls == prior_state_calls + 1);
    assert(test_state.close_calls == 1);
}

int main(void) {
    memset(&test_state, 0, sizeof(test_state));
    test_state.original_dcb.DCBlength = sizeof(test_state.original_dcb);
    test_state.original_dcb.fDtrControl = DTR_CONTROL_DISABLE;
    test_state.original_dcb.fRtsControl = RTS_CONTROL_DISABLE;
    test_state.original_timeouts.ReadIntervalTimeout = 11u;
    test_state.original_timeouts.WriteTotalTimeoutConstant = 22u;

    const h2_pal_serial_host_api_t *api = h2_windows_serial_host_api();
    assert(api != NULL);
    test_ordered_com_discovery(api);
    h2_pal_serial_host_session_t *invalid_session = NULL;
    const h2_pal_uart_io_stream_config_t config = {
        .baud_rate = 230400u,
        .data_bits = 8u,
        .stop_bits = 1u,
        .parity = H2_PAL_UART_PARITY_NONE,
        .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
    };
    assert(h2_pal_serial_host_open(api, "not-a-com-port", &config,
                                   &invalid_session) ==
           H2_PAL_ERR_INVALID_ARG);
    test_partial_io_bounded_timeouts_flush_controls_disconnect_and_close(api);
    return 0;
}
