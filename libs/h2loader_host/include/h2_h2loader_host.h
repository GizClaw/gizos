#ifndef H2_H2LOADER_HOST_H
#define H2_H2LOADER_HOST_H

#include "h2_h2loader_host_catalog.h"
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/os/h2_pal_serial_host.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_H2LOADER_HOST_STATUS_LINE_MAX 4352u
#define H2_H2LOADER_HOST_DEVICE_UID_MAX_LEN 13u
#define H2_H2LOADER_HOST_ENDPOINT_MAX_LEN H2_PAL_SERIAL_HOST_ENDPOINT_MAX_LEN
#define H2_H2LOADER_HOST_DISPLAY_NAME_MAX_LEN 256u
#define H2_H2LOADER_HOST_CANDIDATE_ID_MAX_LEN 512u
#define H2_H2LOADER_HOST_DEFAULT_COMMAND_TIMEOUT_MS 5000u
#define H2_H2LOADER_HOST_WIFI_SCAN_DEFAULT_LIMIT 16u
#define H2_H2LOADER_HOST_WIFI_SCAN_MAX_LIMIT 16u
#define H2_H2LOADER_HOST_WIFI_SCAN_DEFAULT_TIMEOUT_MS 10000u
#define H2_H2LOADER_HOST_WIFI_SCAN_MAX_TIMEOUT_MS 30000u
#define H2_H2LOADER_HOST_RELIABLE_SERIAL_BAUD 460800u
#define H2_H2LOADER_HOST_MFG_STEP_TOTAL 22u
#define H2_H2LOADER_HOST_CAPABILITY_UART (UINT32_C(1) << 0)
#define H2_H2LOADER_HOST_CAPABILITY_WIFI (UINT32_C(1) << 1)
#define H2_H2LOADER_HOST_CAPABILITY_BLE (UINT32_C(1) << 2)
#define H2_H2LOADER_HOST_CAPABILITIES_ALL UINT32_C(0x00000007)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_APP (UINT32_C(1) << 0)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_LOADER (UINT32_C(1) << 1)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_HELP (UINT32_C(1) << 2)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_STATUS (UINT32_C(1) << 3)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_STATS (UINT32_C(1) << 4)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_MEMORY (UINT32_C(1) << 5)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_COREDUMP_STATUS (UINT32_C(1) << 8)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_COREDUMP_DUMP (UINT32_C(1) << 9)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_COREDUMP_ERASE (UINT32_C(1) << 10)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_PAYLOAD (UINT32_C(1) << 11)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_ABORT (UINT32_C(1) << 12)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_URL (UINT32_C(1) << 13)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_SCAN (UINT32_C(1) << 16)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_CONNECT (UINT32_C(1) << 17)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_DISCONNECT (UINT32_C(1) << 18)
#define H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_UPGRADE (UINT32_C(1) << 19)
#define H2_H2LOADER_HOST_COMMAND_AVAILABILITY_ALL \
    (H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_APP | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_LOADER | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_UPGRADE | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_HELP | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_STATUS | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_STATS | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_MEMORY | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_COREDUMP_STATUS | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_COREDUMP_DUMP | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_COREDUMP_ERASE | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_PAYLOAD | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_ABORT | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_URL | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_SCAN | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_CONNECT | \
     H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_DISCONNECT)

typedef enum h2_h2loader_host_transport {
    H2_H2LOADER_HOST_TRANSPORT_SERIAL = 1,
    H2_H2LOADER_HOST_TRANSPORT_BLE = 2,
} h2_h2loader_host_transport_t;

typedef enum h2_h2loader_host_active_role {
    H2_H2LOADER_HOST_ACTIVE_ROLE_UNKNOWN = 0,
    H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER = 1,
    H2_H2LOADER_HOST_ACTIVE_ROLE_APP = 2,
} h2_h2loader_host_active_role_t;

typedef enum h2_h2loader_host_boot_intent {
    H2_H2LOADER_HOST_BOOT_INTENT_UNKNOWN = 0,
    H2_H2LOADER_HOST_BOOT_INTENT_LOADER = 1,
    H2_H2LOADER_HOST_BOOT_INTENT_AUTO = 2,
} h2_h2loader_host_boot_intent_t;

typedef struct h2_h2loader_host_metadata {
    char package_checksum[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
    char image_checksum[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
    char version[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    char board[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    char target[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    uint64_t package_size;
    uint64_t image_size;
    h2_h2loader_host_active_role_t role;
    uint8_t valid;
} h2_h2loader_host_metadata_t;

typedef struct h2_h2loader_host_status {
    char board[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    char target[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    char chip[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    /** Stable physical-device UID used to verify post-reboot BLE identity. */
    char device_uid[H2_H2LOADER_HOST_DEVICE_UID_MAX_LEN];
    char active_version[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    char active_checksum[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
    uint64_t active_image_size;
    h2_h2loader_host_metadata_t stage;
    h2_h2loader_host_metadata_t partition_1;
    h2_h2loader_host_metadata_t partition_2;
    h2_h2loader_host_active_role_t active_role;
    h2_h2loader_host_boot_intent_t boot_intent;
    uint32_t mfg_mode;
    uint8_t mfg_steps[H2_H2LOADER_HOST_MFG_STEP_TOTAL];
    uint32_t capabilities;
    uint32_t command_availability;
    uint32_t running_partition;
    uint32_t next_partition;
    int32_t last;
} h2_h2loader_host_status_t;

h2_h2loader_host_active_role_t h2_h2loader_host_status_active_role(
    const h2_h2loader_host_status_t *status);
uint32_t h2_h2loader_host_status_boot_intent(
    const h2_h2loader_host_status_t *status);
uint32_t h2_h2loader_host_status_mfg_mode(
    const h2_h2loader_host_status_t *status);
uint32_t h2_h2loader_host_status_mfg_step(
    const h2_h2loader_host_status_t *status,
    uint32_t index);

typedef struct h2_h2loader_host_candidate {
    h2_h2loader_host_transport_t transport;
    char candidate_id[H2_H2LOADER_HOST_CANDIDATE_ID_MAX_LEN];
    /** Opaque Host Serial open identifier; empty for BLE candidates. */
    char port_id[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN];
    char endpoint[H2_H2LOADER_HOST_ENDPOINT_MAX_LEN];
    char display_name[H2_H2LOADER_HOST_DISPLAY_NAME_MAX_LEN];
    char advertised_board[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    h2_pal_ble_addr_t ble_address;
    int rssi;
    uint32_t advertised_capabilities;
    uint32_t serial_valid_fields;
    uint32_t serial_capabilities;
    uint16_t usb_vid;
    uint16_t usb_pid;
    uint8_t usb_identity_valid;
    char usb_serial[H2_PAL_SERIAL_HOST_USB_SERIAL_MAX_LEN];
} h2_h2loader_host_candidate_t;

typedef struct h2_h2loader_host_scan_result {
    size_t count;
    size_t required_capacity;
    h2_pal_result_t serial_result;
    h2_pal_result_t ble_result;
} h2_h2loader_host_scan_result_t;

typedef struct h2_h2loader_host_scan_config {
    const h2_pal_serial_host_api_t *serial;
    const h2_pal_ble_host_api_t *ble;
    const h2_pal_sync_api_t *sync;
    const h2_pal_time_api_t *time;
    uint32_t ble_timeout_ms;
    /** Optional exact BLE endpoint that ends scanning as soon as it is seen. */
    const char *ble_endpoint;
    h2_h2loader_host_candidate_t *candidates;
    size_t candidate_capacity;
} h2_h2loader_host_scan_config_t;

/**
 * @brief Discover serial and H2Loader BLE candidates in one bounded call.
 *
 * BLE scanning starts before serial enumeration so both providers overlap.
 * Provider failures are reported independently. Candidates are never merged
 * by display name, advertised board, USB metadata or endpoint.
 */
h2_pal_result_t h2_h2loader_host_scan(
    const h2_h2loader_host_scan_config_t *config,
    h2_h2loader_host_scan_result_t *out_result);

/** Parse one complete, canonical H2_LOADER_STATUS line. */
h2_pal_result_t h2_h2loader_host_status_parse(
    const char *line,
    h2_h2loader_host_status_t *out_status);

/**
 * @brief Verify the final live state for one managed asset.
 *
 * Success requires the active partition metadata and active identity to match
 * the asset's role, version, board, target, image checksum, and source package
 * checksum. Stage must already be finalized and cleared.
 */
h2_pal_result_t h2_h2loader_host_status_verify_asset(
    const h2_h2loader_host_status_t *status,
    const h2_h2loader_host_catalog_entry_t *asset);

typedef struct h2_h2loader_host_serial_connection
    h2_h2loader_host_serial_connection_t;

/** Return nonzero to request cooperative cancellation. */
typedef int (*h2_h2loader_host_cancelled_fn)(void *user);

typedef h2_pal_result_t (*h2_h2loader_host_transport_log_fn)(
    void *user,
    const uint8_t *data,
    size_t len);

typedef struct h2_h2loader_host_serial_connection_config {
    const h2_pal_serial_host_api_t *serial;
    const h2_pal_time_api_t *time;
    const h2_pal_mem_api_t *allocator;
    const char *port_id;
    /** Reliable serial baud; zero selects the 460800 default. */
    uint32_t baud_rate;
    uint32_t handshake_timeout_ms;
    uint32_t command_timeout_ms;
    uint32_t conversation_id;
    /** Optional raw boot marker consumed before opening the reliable session. */
    const char *ready_marker;
    /** Optional bounded delay after command delivery and before response read. */
    uint32_t post_command_delay_ms;
    /** Optional borrowed sink for bytes proven not to belong to a frame. */
    h2_h2loader_host_transport_log_fn on_log;
    void *log_user;
} h2_h2loader_host_serial_connection_config_t;

/**
 * @brief Open a reliable serial H2Loader session and complete SESSION_ACK.
 *
 * After open, the Host deasserts DTR/RTS before borrowing the stream. A
 * canonical unsupported result is accepted for endpoints without control-line
 * support; any other failure aborts the connection. conversation_id must be
 * nonzero or a nonzero value is derived from the monotonic clock. The
 * connection borrows all injected PAL APIs and never falls back to raw
 * transport.
 */
h2_pal_result_t h2_h2loader_host_serial_connect(
    const h2_h2loader_host_serial_connection_config_t *config,
    h2_h2loader_host_serial_connection_t **out_connection);

/** Close a serial connection. Repeating the call with NULL is a no-op. */
h2_pal_result_t h2_h2loader_host_serial_disconnect(
    h2_h2loader_host_serial_connection_t **inout_connection);

/** Run authoritative status over the established reliable session. */
h2_pal_result_t h2_h2loader_host_serial_read_status(
    h2_h2loader_host_serial_connection_t *connection,
    h2_h2loader_host_status_t *out_status);

/** Poll transport logs until cancellation or a physical transport failure. */
h2_pal_result_t h2_h2loader_host_serial_monitor_logs(
    h2_h2loader_host_serial_connection_t *connection,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user);

typedef enum h2_h2loader_host_command {
    H2_H2LOADER_HOST_COMMAND_HELP = 1,
    H2_H2LOADER_HOST_COMMAND_STATUS = 2,
    H2_H2LOADER_HOST_COMMAND_STATS = 3,
    H2_H2LOADER_HOST_COMMAND_MEMORY = 4,
    H2_H2LOADER_HOST_COMMAND_REBOOT_APP = 5,
    H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER = 6,
    H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE = 7,
    H2_H2LOADER_HOST_COMMAND_COREDUMP_STATUS = 9,
    H2_H2LOADER_HOST_COMMAND_COREDUMP_DUMP = 10,
    H2_H2LOADER_HOST_COMMAND_STAGE_ABORT = 11,
    H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT = 14,
    H2_H2LOADER_HOST_COMMAND_WIFI_DISCONNECT = 15,
    H2_H2LOADER_HOST_COMMAND_COREDUMP_ERASE = 17,
    H2_H2LOADER_HOST_COMMAND_STAGE_URL = 18,
    H2_H2LOADER_HOST_COMMAND_WIFI_SCAN = 19,
} h2_h2loader_host_command_t;

/**
 * @brief Consume one borrowed command-output slice synchronously.
 *
 * data is valid only for the callback duration and must be copied before the
 * callback returns. Transport readers deliver newly received chunks while the
 * command is still running; one chunk may contain a partial line or several
 * complete lines. Returning a non-OK result aborts delivery and becomes the
 * execute call's result. The callback runs on the caller's worker thread.
 */
typedef h2_pal_result_t (*h2_h2loader_host_command_output_fn)(
    void *user,
    const uint8_t *data,
    size_t len);

typedef enum h2_h2loader_host_command_terminal {
    H2_H2LOADER_HOST_COMMAND_TERMINAL_NONE = 0,
    H2_H2LOADER_HOST_COMMAND_TERMINAL_OK = 1,
    H2_H2LOADER_HOST_COMMAND_TERMINAL_UNSUPPORTED = 2,
    H2_H2LOADER_HOST_COMMAND_TERMINAL_USAGE = 3,
    H2_H2LOADER_HOST_COMMAND_TERMINAL_ERROR = 4,
} h2_h2loader_host_command_terminal_t;

typedef struct h2_h2loader_host_command_request {
    /** Closed typed command; arbitrary command strings are not accepted. */
    h2_h2loader_host_command_t command;
    /** Borrowed authoritative state, valid for the complete blocking call. */
    const h2_h2loader_host_status_t *status;
    /** Nonzero while another managed operation owns this identity. */
    uint8_t operation_active;
    /** Borrowed bounded parameters used only by the matching typed command. */
    const char *ssid;
    const char *password;
    uint32_t wifi_scan_limit;
    uint32_t wifi_scan_timeout_ms;
    const char *url;
    uint64_t expected_bytes;
    const char *expected_sha256;
    /** Optional cooperative cancellation callback and borrowed context. */
    h2_h2loader_host_cancelled_fn is_cancelled;
    void *cancel_user;
    /** Optional synchronous bounded-output callback and borrowed context. */
    h2_h2loader_host_command_output_fn on_output;
    void *output_user;
} h2_h2loader_host_command_request_t;

typedef struct h2_h2loader_host_command_result {
    /** Transport/callback result before terminal-response interpretation. */
    h2_pal_result_t transport_result;
    /** Parsed marker outcome; NONE means no accepted terminal marker. */
    h2_h2loader_host_command_terminal_t terminal;
    /** Bytes captured by Host Core and offered to on_output. */
    size_t output_bytes;
    /** Nonzero when the fixed terminal-response capture buffer filled.
     * Streaming on_output callbacks still receive every accepted byte. */
    uint8_t output_truncated;
    /** Nonzero when success requires reconnect and live-state verification. */
    uint8_t lifecycle_transition;
} h2_h2loader_host_command_result_t;

/**
 * @brief Check whether one typed command is allowed by authoritative state.
 *
 * The command set is intentionally closed. Arbitrary command strings are not
 * representable here; parameterized commands are validated before transport.
 * The authoritative per-command availability bit is the device-owned gate.
 * Host-owned managed-operation serialization remains an additional gate for
 * lifecycle commands; Host never reconstructs device availability from role,
 * hardware capabilities, or packed state.
 */
h2_pal_result_t h2_h2loader_host_command_validate(
    const h2_h2loader_host_status_t *status,
    uint8_t operation_active,
    h2_h2loader_host_command_t command);

/**
 * @brief Execute one allowed command through a reliable serial session.
 *
 * This is a blocking call bounded by the connection command timeout plus the
 * command-response allowance. Output is delivered through a synchronous
 * callback from the caller's thread; callback data is borrowed. Cancellation
 * is cooperative between bounded I/O steps. The structured result
 * distinguishes terminal acceptance from transport success and reports
 * truncation. For lifecycle commands, H2_PAL_OK proves only terminal command
 * acceptance: the caller must disconnect, rediscover the same authoritative
 * identity and verify final live role/state before presenting success.
 */
h2_pal_result_t h2_h2loader_host_serial_execute_command(
    h2_h2loader_host_serial_connection_t *connection,
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_result_t *out_result);

typedef h2_pal_result_t (*h2_h2loader_host_payload_read_fn)(
    void *user,
    uint64_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_read);

typedef void (*h2_h2loader_host_progress_fn)(
    void *user,
    uint64_t acknowledged_bytes,
    uint64_t total_bytes);

/**
 * @brief Stage one pre-validated package through the reliable session.
 *
 * The package identity, size and checksum come from a validated catalog
 * entry. Success requires both H2_LOADER_STAGE_RECEIVE result=OK and
 * H2_LOADER_STAGE result=OK. Cancellation is checked between bounded writes;
 * interrupted transfers are never replayed automatically.
 */
h2_pal_result_t h2_h2loader_host_serial_stage(
    h2_h2loader_host_serial_connection_t *connection,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user);

/**
 * @brief Request AUTO installation of the staged asset.
 *
 * Both APP and Loader assets use `reboot upgrade`. A returned success only
 * confirms command acceptance. The caller must reconnect and call
 * status_verify_asset before presenting operation success.
 */
h2_pal_result_t h2_h2loader_host_serial_activate(
    h2_h2loader_host_serial_connection_t *connection,
    const h2_h2loader_host_catalog_entry_t *asset);

typedef struct h2_h2loader_host_ble_connection
    h2_h2loader_host_ble_connection_t;

typedef struct h2_h2loader_host_ble_connection_config {
    const h2_pal_ble_host_api_t *ble;
    const h2_pal_task_api_t *task;
    const h2_pal_time_api_t *time;
    const h2_pal_sync_api_t *sync;
    const h2_pal_system_event_api_t *system_event;
    const h2_pal_mem_api_t *allocator;
    h2_pal_ble_addr_t address;
    /**
     * Optional identity copied from H2Loader Service Data.
     *
     * A non-empty value is cross-checked against authoritative status before
     * connect succeeds. Advertising identity is never returned as live state.
     */
    const char *advertised_board;
    uint32_t connect_timeout_ms;
    uint32_t command_timeout_ms;
    /** Optional connection-failure diagnostic sink. Called synchronously on
     * the connect caller's thread with a borrowed complete text line (no NUL
     * included); copy bytes before returning if needed. Not retained after
     * connect returns. A sink error never replaces the original connect error.
     * NULL disables these diagnostics; the library does not write stderr. */
    h2_h2loader_host_transport_log_fn on_log;
    void *log_user;
} h2_h2loader_host_ble_connection_config_t;

/**
 * @brief Connect to one H2Loader BLE endpoint and open its BLE-iKCP stream.
 *
 * The call performs BLE connect, ATT MTU exchange, GATT discovery,
 * notification subscription and an authoritative status probe. A mismatched
 * advertised board fails closed.
 */
h2_pal_result_t h2_h2loader_host_ble_connect(
    const h2_h2loader_host_ble_connection_config_t *config,
    h2_h2loader_host_ble_connection_t **out_connection,
    h2_h2loader_host_status_t *out_status);

h2_pal_result_t h2_h2loader_host_ble_disconnect(
    h2_h2loader_host_ble_connection_t **inout_connection);

h2_pal_result_t h2_h2loader_host_ble_read_status(
    h2_h2loader_host_ble_connection_t *connection,
    h2_h2loader_host_status_t *out_status);

/**
 * @brief Execute one allowed command through the BLE-iKCP stream.
 *
 * Blocking, callback ownership, cancellation, truncation and lifecycle
 * semantics are identical to h2_h2loader_host_serial_execute_command().
 */
h2_pal_result_t h2_h2loader_host_ble_execute_command(
    h2_h2loader_host_ble_connection_t *connection,
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_result_t *out_result);

h2_pal_result_t h2_h2loader_host_ble_stage(
    h2_h2loader_host_ble_connection_t *connection,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user);

h2_pal_result_t h2_h2loader_host_ble_activate(
    h2_h2loader_host_ble_connection_t *connection,
    const h2_h2loader_host_catalog_entry_t *asset);

typedef enum h2_h2loader_host_operation_phase {
    H2_H2LOADER_HOST_OPERATION_CONNECT = 1,
    H2_H2LOADER_HOST_OPERATION_PRECHECK = 2,
    H2_H2LOADER_HOST_OPERATION_STAGE = 3,
    H2_H2LOADER_HOST_OPERATION_ACTIVATE = 4,
    H2_H2LOADER_HOST_OPERATION_REDISCOVER = 5,
    H2_H2LOADER_HOST_OPERATION_FINAL_VERIFY = 6,
    H2_H2LOADER_HOST_OPERATION_COMPLETE = 7,
} h2_h2loader_host_operation_phase_t;

typedef struct h2_h2loader_host_managed_transport_vtable {
    h2_pal_result_t (*connect)(
        void *user,
        h2_h2loader_host_status_t *out_status);
    h2_pal_result_t (*stage)(
        void *user,
        const h2_h2loader_host_catalog_entry_t *asset,
        h2_h2loader_host_payload_read_fn read_payload,
        void *payload_user,
        h2_h2loader_host_cancelled_fn is_cancelled,
        void *cancel_user,
        h2_h2loader_host_progress_fn on_progress,
        void *progress_user);
    h2_pal_result_t (*activate)(
        void *user,
        const h2_h2loader_host_catalog_entry_t *asset);
    /** Read authoritative status without leaving the current connection. */
    h2_pal_result_t (*read_status)(
        void *user,
        h2_h2loader_host_status_t *out_status);
    h2_pal_result_t (*disconnect)(void *user);
    /**
     * Re-enumerate the original discovery candidate.
     *
     * An endpoint or PAL address may select a candidate, but it is not the
     * authoritative physical identity. The following connect/status probe
     * must match the device_uid locked from the device BLE identity MAC.
     * Implementations must never switch to a name-matched candidate.
     */
    h2_pal_result_t (*rediscover)(void *user);
} h2_h2loader_host_managed_transport_vtable_t;

typedef struct h2_h2loader_host_managed_transport {
    void *user;
    const h2_h2loader_host_managed_transport_vtable_t *vtable;
} h2_h2loader_host_managed_transport_t;

typedef void (*h2_h2loader_host_operation_event_fn)(
    void *user,
    h2_h2loader_host_operation_phase_t phase,
    h2_pal_result_t result);

typedef struct h2_h2loader_host_managed_operation_config {
    const h2_pal_time_api_t *time;
    h2_h2loader_host_managed_transport_t transport;
    const h2_h2loader_host_catalog_entry_t *asset;
    h2_h2loader_host_payload_read_fn read_payload;
    void *payload_user;
    h2_h2loader_host_cancelled_fn is_cancelled;
    void *cancel_user;
    h2_h2loader_host_progress_fn on_progress;
    void *progress_user;
    h2_h2loader_host_operation_event_fn on_event;
    void *event_user;
    uint32_t reconnect_delay_ms;
    uint32_t reconnect_attempts;
} h2_h2loader_host_managed_operation_config_t;

/**
 * @brief Run one managed install through a transport-neutral state machine.
 *
 * Success is emitted only after a post-reboot rediscovery, authoritative
 * reconnect and status_verify_asset(). Every successful connect is paired
 * with disconnect, including failure and cancellation paths. A CLOSED or
 * TIMEOUT during Stage permits one same-identity reconnect: an already durable
 * matching Stage is accepted, otherwise an empty Stage is retransmitted once.
 */
h2_pal_result_t h2_h2loader_host_managed_operation_run(
    const h2_h2loader_host_managed_operation_config_t *config,
    h2_h2loader_host_status_t *out_final_status);

/**
 * @brief Stage an asset and verify the published candidate after reconnect.
 *
 * This transport-neutral operation does not activate the asset. Success
 * requires rediscovery of the original candidate, matching board/target and
 * exact staged bytes/package checksum. Every successful connect is paired
 * with disconnect on success, cancellation and failure paths. A CLOSED or
 * TIMEOUT during Stage permits one same-identity reconnect: an already durable
 * matching Stage is accepted, otherwise an empty Stage is retransmitted once.
 */
h2_pal_result_t h2_h2loader_host_stage_operation_run(
    const h2_h2loader_host_managed_operation_config_t *config,
    h2_h2loader_host_status_t *out_final_status);

#ifdef __cplusplus
}
#endif

#endif
