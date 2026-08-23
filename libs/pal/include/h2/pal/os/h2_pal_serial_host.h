#ifndef H2_PAL_SERIAL_HOST_H
#define H2_PAL_SERIAL_HOST_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/hal/h2_pal_uart_io_stream.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN 512u
#define H2_PAL_SERIAL_HOST_ENDPOINT_MAX_LEN 512u
#define H2_PAL_SERIAL_HOST_DISPLAY_NAME_MAX_LEN 256u
#define H2_PAL_SERIAL_HOST_USB_SERIAL_MAX_LEN 256u
#define H2_PAL_SERIAL_HOST_FLUSH_TIMEOUT_MS 5000u

/** Optional fields populated in h2_pal_serial_host_port_info_t. */
typedef enum h2_pal_serial_host_port_field {
    H2_PAL_SERIAL_HOST_PORT_FIELD_DISPLAY_NAME = 1u << 0,
    H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID = 1u << 1,
    H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID = 1u << 2,
    H2_PAL_SERIAL_HOST_PORT_FIELD_USB_SERIAL = 1u << 3,
} h2_pal_serial_host_port_field_t;

/** Operations supported by a serial endpoint. */
typedef enum h2_pal_serial_host_capability {
    H2_PAL_SERIAL_HOST_CAP_DTR = 1u << 0,
    H2_PAL_SERIAL_HOST_CAP_RTS = 1u << 1,
} h2_pal_serial_host_capability_t;

/** Modem/control lines that may be queried or changed explicitly. */
typedef enum h2_pal_serial_host_control_line {
    H2_PAL_SERIAL_HOST_CONTROL_DTR = 1u << 0,
    H2_PAL_SERIAL_HOST_CONTROL_RTS = 1u << 1,
} h2_pal_serial_host_control_line_t;

/**
 * @brief One immutable entry copied from a point-in-time enumeration snapshot.
 *
 * port_id is the opaque value passed back to open(). endpoint is the current
 * user-visible platform endpoint. Both are always non-empty and NUL
 * terminated. Optional values are valid only when their corresponding bit is
 * present in valid_fields; absent values are zeroed rather than fabricated.
 * An endpoint or USB identity is not an authoritative board identity.
 */
typedef struct h2_pal_serial_host_port_info {
    char port_id[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN];
    char endpoint[H2_PAL_SERIAL_HOST_ENDPOINT_MAX_LEN];
    char display_name[H2_PAL_SERIAL_HOST_DISPLAY_NAME_MAX_LEN];
    char usb_serial[H2_PAL_SERIAL_HOST_USB_SERIAL_MAX_LEN];
    uint32_t valid_fields;
    uint32_t capabilities;
    uint16_t usb_vid;
    uint16_t usb_pid;
} h2_pal_serial_host_port_info_t;

typedef struct h2_pal_serial_host_snapshot h2_pal_serial_host_snapshot_t;
typedef struct h2_pal_serial_host_session h2_pal_serial_host_session_t;

typedef struct h2_pal_serial_host_vtable {
    h2_pal_result_t (*scan)(
        void *user,
        h2_pal_serial_host_snapshot_t **out_snapshot);
    h2_pal_result_t (*snapshot_count)(
        void *user,
        const h2_pal_serial_host_snapshot_t *snapshot,
        size_t *out_count);
    h2_pal_result_t (*snapshot_get)(
        void *user,
        const h2_pal_serial_host_snapshot_t *snapshot,
        size_t index,
        h2_pal_serial_host_port_info_t *out_info);
    h2_pal_result_t (*snapshot_destroy)(
        void *user,
        h2_pal_serial_host_snapshot_t **inout_snapshot);
    h2_pal_result_t (*open)(
        void *user,
        const char *port_id,
        const h2_pal_uart_io_stream_config_t *config,
        h2_pal_serial_host_session_t **out_session);
    h2_pal_result_t (*session_stream)(
        void *user,
        h2_pal_serial_host_session_t *session,
        const h2_pal_uart_io_stream_api_t **out_stream);
    h2_pal_result_t (*set_control_lines)(
        void *user,
        h2_pal_serial_host_session_t *session,
        uint32_t line_mask,
        uint32_t asserted_lines);
    h2_pal_result_t (*get_control_lines)(
        void *user,
        h2_pal_serial_host_session_t *session,
        uint32_t *out_asserted_lines);
    h2_pal_result_t (*close)(
        void *user,
        h2_pal_serial_host_session_t **inout_session);
} h2_pal_serial_host_vtable_t;

typedef struct h2_pal_serial_host_api {
    void *user;
    const h2_pal_serial_host_vtable_t *vtable;
} h2_pal_serial_host_api_t;

/**
 * @brief Capture the currently enumerable serial endpoints.
 *
 * The returned snapshot and all strings it contains are owned by the caller
 * until snapshot_destroy(). A later scan is independent and may observe
 * insertion, removal, or a changed endpoint.
 */
static inline h2_pal_result_t h2_pal_serial_host_scan(
    const h2_pal_serial_host_api_t *api,
    h2_pal_serial_host_snapshot_t **out_snapshot) {
    if (out_snapshot != NULL) {
        *out_snapshot = NULL;
    }
    if (out_snapshot == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->scan == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->scan(api->user, out_snapshot);
}

static inline h2_pal_result_t h2_pal_serial_host_snapshot_count(
    const h2_pal_serial_host_api_t *api,
    const h2_pal_serial_host_snapshot_t *snapshot,
    size_t *out_count) {
    if (out_count != NULL) {
        *out_count = 0u;
    }
    if (snapshot == NULL || out_count == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->snapshot_count == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->snapshot_count(api->user, snapshot, out_count);
}

static inline h2_pal_result_t h2_pal_serial_host_snapshot_get(
    const h2_pal_serial_host_api_t *api,
    const h2_pal_serial_host_snapshot_t *snapshot,
    size_t index,
    h2_pal_serial_host_port_info_t *out_info) {
    if (out_info != NULL) {
        memset(out_info, 0, sizeof(*out_info));
    }
    if (snapshot == NULL || out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->snapshot_get == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->snapshot_get(api->user, snapshot, index, out_info);
}

/** Destroy a snapshot. Calling this again with a NULL handle is a no-op. */
static inline h2_pal_result_t h2_pal_serial_host_snapshot_destroy(
    const h2_pal_serial_host_api_t *api,
    h2_pal_serial_host_snapshot_t **inout_snapshot) {
    if (inout_snapshot == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (*inout_snapshot == NULL) {
        return H2_PAL_OK;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->snapshot_destroy == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->snapshot_destroy(api->user, inout_snapshot);
}

/**
 * @brief Open and configure one independent serial session.
 *
 * A normal open does not toggle DTR/RTS, reset a device, or discard input.
 * Busy and permission failures return H2_PAL_ERR_UNAVAILABLE. A stale port_id
 * returns H2_PAL_ERR_NOT_FOUND. The config is copied synchronously.
 */
static inline h2_pal_result_t h2_pal_serial_host_open(
    const h2_pal_serial_host_api_t *api,
    const char *port_id,
    const h2_pal_uart_io_stream_config_t *config,
    h2_pal_serial_host_session_t **out_session) {
    if (out_session != NULL) {
        *out_session = NULL;
    }
    if (port_id == NULL || port_id[0] == '\0' || config == NULL ||
        out_session == NULL || config->baud_rate == 0u ||
        config->data_bits < 5u || config->data_bits > 8u ||
        (config->stop_bits != 1u && config->stop_bits != 2u) ||
        (config->parity != H2_PAL_UART_PARITY_NONE &&
         config->parity != H2_PAL_UART_PARITY_EVEN &&
         config->parity != H2_PAL_UART_PARITY_ODD) ||
        (config->flow_control & ~H2_PAL_UART_FLOW_CONTROL_RTS_CTS) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->open == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->open(api->user, port_id, config, out_session);
}

/**
 * @brief Borrow the UART byte-stream view owned by a live session.
 *
 * The stream remains valid until close() returns. Its read and write calls are
 * bounded by timeout_ms and may report partial progress. Distinct sessions are
 * safe to use concurrently. A backend serializes calls on one session; close
 * waits for an in-flight bounded operation before releasing resources.
 * Callers must prevent new operations from starting once close begins.
 */
static inline h2_pal_result_t h2_pal_serial_host_session_stream(
    const h2_pal_serial_host_api_t *api,
    h2_pal_serial_host_session_t *session,
    const h2_pal_uart_io_stream_api_t **out_stream) {
    if (out_stream != NULL) {
        *out_stream = NULL;
    }
    if (session == NULL || out_stream == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->session_stream == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->session_stream(api->user, session, out_stream);
}

static inline h2_pal_result_t h2_pal_serial_host_set_control_lines(
    const h2_pal_serial_host_api_t *api,
    h2_pal_serial_host_session_t *session,
    uint32_t line_mask,
    uint32_t asserted_lines) {
    const uint32_t valid_lines =
        H2_PAL_SERIAL_HOST_CONTROL_DTR | H2_PAL_SERIAL_HOST_CONTROL_RTS;
    if (session == NULL || line_mask == 0u || (line_mask & ~valid_lines) != 0u ||
        (asserted_lines & ~line_mask) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->set_control_lines == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->set_control_lines(
        api->user, session, line_mask, asserted_lines);
}

static inline h2_pal_result_t h2_pal_serial_host_get_control_lines(
    const h2_pal_serial_host_api_t *api,
    h2_pal_serial_host_session_t *session,
    uint32_t *out_asserted_lines) {
    if (out_asserted_lines != NULL) {
        *out_asserted_lines = 0u;
    }
    if (session == NULL || out_asserted_lines == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->get_control_lines == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_control_lines(
        api->user, session, out_asserted_lines);
}

/**
 * @brief Close a session, null the caller's handle, and release the endpoint.
 *
 * Calling close again with the resulting NULL handle is a no-op. A caller must
 * synchronize access to the handle itself when close and I/O originate on
 * different threads.
 */
static inline h2_pal_result_t h2_pal_serial_host_close(
    const h2_pal_serial_host_api_t *api,
    h2_pal_serial_host_session_t **inout_session) {
    if (inout_session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (*inout_session == NULL) {
        return H2_PAL_OK;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->close == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->close(api->user, inout_session);
}

#ifdef __cplusplus
}
#endif

#endif
