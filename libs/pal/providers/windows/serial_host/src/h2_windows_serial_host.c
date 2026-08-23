#include "h2_windows_serial_host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define H2_WINDOWS_SERIAL_MIN_WRITE_TIMEOUT_MS 1u

#ifdef H2_WINDOWS_SERIAL_HOST_TESTING
DWORD WINAPI h2_test_QueryDosDeviceW(LPCWSTR, LPWSTR, DWORD);
HANDLE WINAPI h2_test_CreateFileW(LPCWSTR, DWORD, DWORD,
                                  LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
BOOL WINAPI h2_test_GetCommState(HANDLE, LPDCB);
BOOL WINAPI h2_test_GetCommTimeouts(HANDLE, LPCOMMTIMEOUTS);
BOOL WINAPI h2_test_SetCommState(HANDLE, LPDCB);
BOOL WINAPI h2_test_SetupComm(HANDLE, DWORD, DWORD);
BOOL WINAPI h2_test_SetCommTimeouts(HANDLE, LPCOMMTIMEOUTS);
BOOL WINAPI h2_test_ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL WINAPI h2_test_WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL WINAPI h2_test_ClearCommError(HANDLE, LPDWORD, LPCOMSTAT);
ULONGLONG WINAPI h2_test_GetTickCount64(void);
VOID WINAPI h2_test_Sleep(DWORD);
BOOL WINAPI h2_test_EscapeCommFunction(HANDLE, DWORD);
BOOL WINAPI h2_test_CloseHandle(HANDLE);
#endif

#ifndef H2_WINDOWS_CLEAR_COMM_ERROR
#define H2_WINDOWS_CLEAR_COMM_ERROR ClearCommError
#endif
#ifndef H2_WINDOWS_CLOSE_HANDLE
#define H2_WINDOWS_CLOSE_HANDLE CloseHandle
#endif
#ifndef H2_WINDOWS_CREATE_FILE
#define H2_WINDOWS_CREATE_FILE CreateFileW
#endif
#ifndef H2_WINDOWS_ESCAPE_COMM_FUNCTION
#define H2_WINDOWS_ESCAPE_COMM_FUNCTION EscapeCommFunction
#endif
#ifndef H2_WINDOWS_GET_COMM_STATE
#define H2_WINDOWS_GET_COMM_STATE GetCommState
#endif
#ifndef H2_WINDOWS_GET_COMM_TIMEOUTS
#define H2_WINDOWS_GET_COMM_TIMEOUTS GetCommTimeouts
#endif
#ifndef H2_WINDOWS_GET_TICK_COUNT
#define H2_WINDOWS_GET_TICK_COUNT GetTickCount64
#endif
#ifndef H2_WINDOWS_QUERY_DOS_DEVICE
#define H2_WINDOWS_QUERY_DOS_DEVICE QueryDosDeviceW
#endif
#ifndef H2_WINDOWS_READ_FILE
#define H2_WINDOWS_READ_FILE ReadFile
#endif
#ifndef H2_WINDOWS_SET_COMM_STATE
#define H2_WINDOWS_SET_COMM_STATE SetCommState
#endif
#ifndef H2_WINDOWS_SET_COMM_TIMEOUTS
#define H2_WINDOWS_SET_COMM_TIMEOUTS SetCommTimeouts
#endif
#ifndef H2_WINDOWS_SETUP_COMM
#define H2_WINDOWS_SETUP_COMM SetupComm
#endif
#ifndef H2_WINDOWS_SLEEP
#define H2_WINDOWS_SLEEP Sleep
#endif
#ifndef H2_WINDOWS_WRITE_FILE
#define H2_WINDOWS_WRITE_FILE WriteFile
#endif

struct h2_pal_serial_host_snapshot {
    h2_pal_serial_host_port_info_t *ports;
    size_t count;
};

struct h2_pal_serial_host_session {
    HANDLE handle;
    CRITICAL_SECTION lock;
    DCB original_dcb;
    COMMTIMEOUTS original_timeouts;
    int has_original_dcb;
    int has_original_timeouts;
    int closed;
    uint32_t asserted_lines;
    h2_pal_uart_io_stream_api_t stream;
};

static h2_pal_result_t windows_serial_configure(
    void *user,
    const h2_pal_uart_io_stream_config_t *config);
static h2_pal_result_t windows_serial_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms);
static h2_pal_result_t windows_serial_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms);
static h2_pal_result_t windows_serial_flush(void *user);

static const h2_pal_uart_io_stream_vtable_t windows_serial_stream_vtable = {
    .configure = windows_serial_configure,
    .read = windows_serial_read,
    .write = windows_serial_write,
    .flush = windows_serial_flush,
};

static h2_pal_result_t windows_serial_error(DWORD error) {
    switch (error) {
        case ERROR_SUCCESS:
            return H2_PAL_OK;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
            return H2_PAL_ERR_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return H2_PAL_ERR_UNAVAILABLE;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return H2_PAL_ERR_NO_MEMORY;
        case ERROR_SEM_TIMEOUT:
        case ERROR_TIMEOUT:
            return H2_PAL_ERR_TIMEOUT;
        case ERROR_OPERATION_ABORTED:
        case ERROR_INVALID_HANDLE:
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_GEN_FAILURE:
            return H2_PAL_ERR_CLOSED;
        case ERROR_INVALID_PARAMETER:
            return H2_PAL_ERR_INVALID_ARG;
        default:
            return H2_PAL_ERR_IO;
    }
}

static int windows_serial_port_id_valid(const char *port_id) {
    if (port_id == NULL ||
        (port_id[0] != 'C' && port_id[0] != 'c')) {
        return 0;
    }
    if (port_id[1] == '\0' ||
        (port_id[1] != 'O' && port_id[1] != 'o')) {
        return 0;
    }
    if (port_id[2] == '\0' ||
        (port_id[2] != 'M' && port_id[2] != 'm') ||
        port_id[3] == '\0') {
        return 0;
    }
    for (size_t index = 3u; port_id[index] != '\0'; ++index) {
        if (!isdigit((unsigned char)port_id[index])) {
            return 0;
        }
    }
    return 1;
}

static int windows_serial_compare_ports(const void *left, const void *right) {
    const h2_pal_serial_host_port_info_t *left_port = left;
    const h2_pal_serial_host_port_info_t *right_port = right;
    unsigned long left_number = strtoul(left_port->port_id + 3, NULL, 10);
    unsigned long right_number = strtoul(right_port->port_id + 3, NULL, 10);
    if (left_number < right_number) {
        return -1;
    }
    if (left_number > right_number) {
        return 1;
    }
    return strcmp(left_port->port_id, right_port->port_id);
}

static h2_pal_result_t windows_serial_snapshot_append(
    h2_pal_serial_host_snapshot_t *snapshot,
    const char *port_id) {
    if (snapshot->count == SIZE_MAX / sizeof(*snapshot->ports)) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_pal_serial_host_port_info_t *ports = realloc(
        snapshot->ports, (snapshot->count + 1u) * sizeof(*snapshot->ports));
    if (ports == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    snapshot->ports = ports;
    h2_pal_serial_host_port_info_t *info = &ports[snapshot->count];
    memset(info, 0, sizeof(*info));
    size_t length = strlen(port_id);
    if (length >= sizeof(info->port_id) || length >= sizeof(info->endpoint)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(info->port_id, port_id, length + 1u);
    memcpy(info->endpoint, port_id, length + 1u);
    info->capabilities =
        H2_PAL_SERIAL_HOST_CAP_DTR | H2_PAL_SERIAL_HOST_CAP_RTS;
    ++snapshot->count;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_serial_scan(
    void *user,
    h2_pal_serial_host_snapshot_t **out_snapshot) {
    (void)user;
    if (out_snapshot == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_snapshot = NULL;
    h2_pal_serial_host_snapshot_t *snapshot = calloc(1u, sizeof(*snapshot));
    if (snapshot == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }

    DWORD capacity = 4096u;
    wchar_t *devices = NULL;
    DWORD copied = 0u;
    for (;;) {
        if ((size_t)capacity > SIZE_MAX / sizeof(*devices)) {
            free(snapshot);
            return H2_PAL_ERR_NO_MEMORY;
        }
        wchar_t *resized = realloc(devices, (size_t)capacity * sizeof(*devices));
        if (resized == NULL) {
            free(devices);
            free(snapshot);
            return H2_PAL_ERR_NO_MEMORY;
        }
        devices = resized;
        copied = H2_WINDOWS_QUERY_DOS_DEVICE(NULL, devices, capacity);
        if (copied != 0u) {
            break;
        }
        DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER || capacity > MAXDWORD / 2u) {
            free(devices);
            free(snapshot);
            return windows_serial_error(error);
        }
        capacity *= 2u;
    }

    h2_pal_result_t result = H2_PAL_OK;
    for (const wchar_t *name = devices; *name != L'\0';
         name += wcslen(name) + 1u) {
        size_t length = wcslen(name);
        if (length < 4u || length >= H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN ||
            (name[0] != L'C' && name[0] != L'c') ||
            (name[1] != L'O' && name[1] != L'o') ||
            (name[2] != L'M' && name[2] != L'm')) {
            continue;
        }
        char port_id[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN];
        port_id[0] = 'C';
        port_id[1] = 'O';
        port_id[2] = 'M';
        size_t index = 3u;
        for (; index < length && name[index] >= L'0' && name[index] <= L'9';
             ++index) {
            port_id[index] = (char)name[index];
        }
        if (index != length || index == 3u) {
            continue;
        }
        port_id[index] = '\0';
        result = windows_serial_snapshot_append(snapshot, port_id);
        if (result != H2_PAL_OK) {
            break;
        }
    }
    free(devices);
    if (result != H2_PAL_OK) {
        free(snapshot->ports);
        free(snapshot);
        return result;
    }
    if (snapshot->count > 1u) {
        qsort(snapshot->ports, snapshot->count, sizeof(*snapshot->ports),
              windows_serial_compare_ports);
    }
    *out_snapshot = snapshot;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_serial_snapshot_count(
    void *user,
    const h2_pal_serial_host_snapshot_t *snapshot,
    size_t *out_count) {
    (void)user;
    if (snapshot == NULL || out_count == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_count = snapshot->count;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_serial_snapshot_get(
    void *user,
    const h2_pal_serial_host_snapshot_t *snapshot,
    size_t index,
    h2_pal_serial_host_port_info_t *out_info) {
    (void)user;
    if (snapshot == NULL || out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (index >= snapshot->count) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_info = snapshot->ports[index];
    return H2_PAL_OK;
}

static h2_pal_result_t windows_serial_snapshot_destroy(
    void *user,
    h2_pal_serial_host_snapshot_t **inout_snapshot) {
    (void)user;
    if (inout_snapshot == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (*inout_snapshot != NULL) {
        free((*inout_snapshot)->ports);
        free(*inout_snapshot);
        *inout_snapshot = NULL;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t windows_serial_configure_locked(
    h2_pal_serial_host_session_t *session,
    const h2_pal_uart_io_stream_config_t *config) {
    if (session->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (config->baud_rate == 0u || config->data_bits < 5u ||
        config->data_bits > 8u ||
        (config->stop_bits != 1u && config->stop_bits != 2u) ||
        (config->parity != H2_PAL_UART_PARITY_NONE &&
         config->parity != H2_PAL_UART_PARITY_EVEN &&
         config->parity != H2_PAL_UART_PARITY_ODD) ||
        (config->flow_control & ~H2_PAL_UART_FLOW_CONTROL_RTS_CTS) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    DCB dcb = {0};
    dcb.DCBlength = (DWORD)sizeof(dcb);
    if (!H2_WINDOWS_GET_COMM_STATE(session->handle, &dcb)) {
        return windows_serial_error(GetLastError());
    }
    dcb.BaudRate = config->baud_rate;
    dcb.ByteSize = config->data_bits;
    dcb.StopBits = config->stop_bits == 2u ? TWOSTOPBITS : ONESTOPBIT;
    dcb.Parity = NOPARITY;
    dcb.fParity = FALSE;
    if (config->parity == H2_PAL_UART_PARITY_EVEN) {
        dcb.Parity = EVENPARITY;
        dcb.fParity = TRUE;
    } else if (config->parity == H2_PAL_UART_PARITY_ODD) {
        dcb.Parity = ODDPARITY;
        dcb.fParity = TRUE;
    }
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow =
        (config->flow_control & H2_PAL_UART_FLOW_CONTROL_CTS) != 0u;
    if ((config->flow_control & H2_PAL_UART_FLOW_CONTROL_RTS) != 0u) {
        dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
    } else {
        dcb.fRtsControl =
            (session->asserted_lines & H2_PAL_SERIAL_HOST_CONTROL_RTS) != 0u
                ? RTS_CONTROL_ENABLE
                : RTS_CONTROL_DISABLE;
    }
    if ((config->rx_buffer_size != 0u || config->tx_buffer_size != 0u) &&
        !H2_WINDOWS_SETUP_COMM(session->handle,
                   config->rx_buffer_size == 0u
                       ? 4096u
                       : config->rx_buffer_size > MAXDWORD
                       ? MAXDWORD
                       : (DWORD)config->rx_buffer_size,
                   config->tx_buffer_size == 0u
                       ? 4096u
                       : config->tx_buffer_size > MAXDWORD
                       ? MAXDWORD
                       : (DWORD)config->tx_buffer_size)) {
        return windows_serial_error(GetLastError());
    }
    if (!H2_WINDOWS_SET_COMM_STATE(session->handle, &dcb)) {
        return windows_serial_error(GetLastError());
    }
    return H2_PAL_OK;
}

static h2_pal_result_t windows_serial_configure(
    void *user,
    const h2_pal_uart_io_stream_config_t *config) {
    h2_pal_serial_host_session_t *session = user;
    if (session == NULL || config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    EnterCriticalSection(&session->lock);
    h2_pal_result_t result = windows_serial_configure_locked(session, config);
    LeaveCriticalSection(&session->lock);
    return result;
}

static h2_pal_result_t windows_serial_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    h2_pal_serial_host_session_t *session = user;
    if (session == NULL || buffer == NULL || out_read == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_read = 0u;
    if (len == 0u) {
        return H2_PAL_OK;
    }
    EnterCriticalSection(&session->lock);
    if (session->closed) {
        LeaveCriticalSection(&session->lock);
        return H2_PAL_ERR_CLOSED;
    }
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = timeout_ms;
    DWORD count = 0u;
    h2_pal_result_t result = H2_PAL_OK;
    if (!H2_WINDOWS_SET_COMM_TIMEOUTS(session->handle, &timeouts)) {
        result = windows_serial_error(GetLastError());
    } else if (!H2_WINDOWS_READ_FILE(
                   session->handle, buffer,
                   len > MAXDWORD ? MAXDWORD : (DWORD)len, &count, NULL)) {
        result = windows_serial_error(GetLastError());
    } else if (count == 0u) {
        result = H2_PAL_ERR_TIMEOUT;
    } else {
        *out_read = (size_t)count;
    }
    LeaveCriticalSection(&session->lock);
    return result;
}

static h2_pal_result_t windows_serial_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    h2_pal_serial_host_session_t *session = user;
    if (session == NULL || buffer == NULL || out_written == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_written = 0u;
    if (len == 0u) {
        return H2_PAL_OK;
    }
    EnterCriticalSection(&session->lock);
    if (session->closed) {
        LeaveCriticalSection(&session->lock);
        return H2_PAL_ERR_CLOSED;
    }
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.WriteTotalTimeoutConstant = timeout_ms == 0u
        ? H2_WINDOWS_SERIAL_MIN_WRITE_TIMEOUT_MS
        : timeout_ms;
    DWORD count = 0u;
    h2_pal_result_t result = H2_PAL_OK;
    if (!H2_WINDOWS_SET_COMM_TIMEOUTS(session->handle, &timeouts)) {
        result = windows_serial_error(GetLastError());
    } else if (!H2_WINDOWS_WRITE_FILE(
                   session->handle, buffer,
                   len > MAXDWORD ? MAXDWORD : (DWORD)len, &count, NULL)) {
        result = windows_serial_error(GetLastError());
    } else if (count == 0u) {
        result = H2_PAL_ERR_TIMEOUT;
    } else {
        *out_written = (size_t)count;
    }
    LeaveCriticalSection(&session->lock);
    return result;
}

static h2_pal_result_t windows_serial_flush(void *user) {
    h2_pal_serial_host_session_t *session = user;
    if (session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    EnterCriticalSection(&session->lock);
    h2_pal_result_t result = H2_PAL_OK;
    ULONGLONG deadline =
        H2_WINDOWS_GET_TICK_COUNT() + H2_PAL_SERIAL_HOST_FLUSH_TIMEOUT_MS;
    while (!session->closed) {
        COMSTAT status = {0};
        DWORD errors = 0u;
        if (!H2_WINDOWS_CLEAR_COMM_ERROR(session->handle, &errors, &status)) {
            result = windows_serial_error(GetLastError());
            break;
        }
        if (status.cbOutQue == 0u) {
            break;
        }
        if (H2_WINDOWS_GET_TICK_COUNT() >= deadline) {
            result = H2_PAL_ERR_TIMEOUT;
            break;
        }
        H2_WINDOWS_SLEEP(1u);
    }
    if (session->closed) {
        result = H2_PAL_ERR_CLOSED;
    }
    LeaveCriticalSection(&session->lock);
    return result;
}

static h2_pal_result_t windows_serial_open(
    void *user,
    const char *port_id,
    const h2_pal_uart_io_stream_config_t *config,
    h2_pal_serial_host_session_t **out_session) {
    (void)user;
    if (!windows_serial_port_id_valid(port_id) || config == NULL ||
        out_session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_session = NULL;
    char endpoint[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN + 5u];
    int endpoint_length = snprintf(endpoint, sizeof(endpoint), "\\\\.\\%s", port_id);
    if (endpoint_length < 0 || (size_t)endpoint_length >= sizeof(endpoint)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    wchar_t wide_endpoint[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN + 5u];
    int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          endpoint, -1, wide_endpoint,
                                          (int)(sizeof(wide_endpoint) /
                                                sizeof(wide_endpoint[0])));
    if (wide_length == 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    HANDLE handle = H2_WINDOWS_CREATE_FILE(
        wide_endpoint, GENERIC_READ | GENERIC_WRITE, 0u, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return windows_serial_error(GetLastError());
    }
    h2_pal_serial_host_session_t *session = calloc(1u, sizeof(*session));
    if (session == NULL) {
        (void)H2_WINDOWS_CLOSE_HANDLE(handle);
        return H2_PAL_ERR_NO_MEMORY;
    }
    session->handle = handle;
    session->stream.user = session;
    session->stream.vtable = &windows_serial_stream_vtable;
    InitializeCriticalSection(&session->lock);
    session->original_dcb.DCBlength =
        (DWORD)sizeof(session->original_dcb);
    session->has_original_dcb =
        H2_WINDOWS_GET_COMM_STATE(handle, &session->original_dcb);
    session->has_original_timeouts =
        H2_WINDOWS_GET_COMM_TIMEOUTS(handle, &session->original_timeouts);
    if (session->has_original_dcb) {
        if (session->original_dcb.fDtrControl == DTR_CONTROL_ENABLE) {
            session->asserted_lines |= H2_PAL_SERIAL_HOST_CONTROL_DTR;
        }
        if (session->original_dcb.fRtsControl == RTS_CONTROL_ENABLE) {
            session->asserted_lines |= H2_PAL_SERIAL_HOST_CONTROL_RTS;
        }
    }
    h2_pal_result_t result = windows_serial_configure_locked(session, config);
    if (result != H2_PAL_OK) {
        if (session->has_original_timeouts) {
            (void)H2_WINDOWS_SET_COMM_TIMEOUTS(handle,
                                                &session->original_timeouts);
        }
        if (session->has_original_dcb) {
            (void)H2_WINDOWS_SET_COMM_STATE(handle, &session->original_dcb);
        }
        DeleteCriticalSection(&session->lock);
        (void)H2_WINDOWS_CLOSE_HANDLE(handle);
        free(session);
        return result;
    }
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_serial_session_stream(
    void *user,
    h2_pal_serial_host_session_t *session,
    const h2_pal_uart_io_stream_api_t **out_stream) {
    (void)user;
    if (session == NULL || out_stream == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    EnterCriticalSection(&session->lock);
    h2_pal_result_t result = session->closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
    if (result == H2_PAL_OK) {
        *out_stream = &session->stream;
    }
    LeaveCriticalSection(&session->lock);
    return result;
}

static h2_pal_result_t windows_serial_set_control_lines(
    void *user,
    h2_pal_serial_host_session_t *session,
    uint32_t line_mask,
    uint32_t asserted_lines) {
    (void)user;
    if (session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    EnterCriticalSection(&session->lock);
    h2_pal_result_t result = session->closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
    const uint32_t lines[] = {
        H2_PAL_SERIAL_HOST_CONTROL_DTR,
        H2_PAL_SERIAL_HOST_CONTROL_RTS,
    };
    const DWORD set_operations[] = {SETDTR, SETRTS};
    const DWORD clear_operations[] = {CLRDTR, CLRRTS};
    for (size_t index = 0u; result == H2_PAL_OK && index < 2u; ++index) {
        if ((line_mask & lines[index]) == 0u) {
            continue;
        }
        DWORD operation = (asserted_lines & lines[index]) != 0u
                              ? set_operations[index]
                              : clear_operations[index];
        if (!H2_WINDOWS_ESCAPE_COMM_FUNCTION(session->handle, operation)) {
            result = windows_serial_error(GetLastError());
        } else if ((asserted_lines & lines[index]) != 0u) {
            session->asserted_lines |= lines[index];
        } else {
            session->asserted_lines &= ~lines[index];
        }
    }
    LeaveCriticalSection(&session->lock);
    return result;
}

static h2_pal_result_t windows_serial_get_control_lines(
    void *user,
    h2_pal_serial_host_session_t *session,
    uint32_t *out_asserted_lines) {
    (void)user;
    if (session == NULL || out_asserted_lines == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    EnterCriticalSection(&session->lock);
    h2_pal_result_t result = session->closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
    *out_asserted_lines = result == H2_PAL_OK ? session->asserted_lines : 0u;
    LeaveCriticalSection(&session->lock);
    return result;
}

static h2_pal_result_t windows_serial_close(
    void *user,
    h2_pal_serial_host_session_t **inout_session) {
    (void)user;
    if (inout_session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_serial_host_session_t *session = *inout_session;
    if (session == NULL) {
        return H2_PAL_OK;
    }
    EnterCriticalSection(&session->lock);
    session->closed = 1;
    h2_pal_result_t result = H2_PAL_OK;
    if (session->has_original_timeouts &&
        !H2_WINDOWS_SET_COMM_TIMEOUTS(session->handle,
                                      &session->original_timeouts)) {
        result = windows_serial_error(GetLastError());
    }
    if (session->has_original_dcb &&
        !H2_WINDOWS_SET_COMM_STATE(session->handle, &session->original_dcb) &&
        result == H2_PAL_OK) {
        result = windows_serial_error(GetLastError());
    }
    if (!H2_WINDOWS_CLOSE_HANDLE(session->handle) && result == H2_PAL_OK) {
        result = windows_serial_error(GetLastError());
    }
    session->handle = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&session->lock);
    DeleteCriticalSection(&session->lock);
    free(session);
    *inout_session = NULL;
    return result;
}

static const h2_pal_serial_host_vtable_t windows_serial_host_vtable = {
    .scan = windows_serial_scan,
    .snapshot_count = windows_serial_snapshot_count,
    .snapshot_get = windows_serial_snapshot_get,
    .snapshot_destroy = windows_serial_snapshot_destroy,
    .open = windows_serial_open,
    .session_stream = windows_serial_session_stream,
    .set_control_lines = windows_serial_set_control_lines,
    .get_control_lines = windows_serial_get_control_lines,
    .close = windows_serial_close,
};

static const h2_pal_serial_host_api_t windows_serial_host_api = {
    .user = NULL,
    .vtable = &windows_serial_host_vtable,
};

const h2_pal_serial_host_api_t *h2_windows_serial_host_api(void) {
    return &windows_serial_host_api;
}
