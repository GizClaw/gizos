#ifndef H2_LOADER_BLE_H
#define H2_LOADER_BLE_H

#include "h2_bleikcp.h"
#include "h2_command.h"
#include "h2_loader_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LOADER_BLE_PROTOCOL_VERSION 1u
#define H2_LOADER_BLE_COMPACT_PROTOCOL_VERSION 2u
#define H2_LOADER_BLE_BOARD_MAX 63u
#define H2_LOADER_BLE_INLINE_BOARD_MAX 32u

typedef enum h2_loader_ble_advertising_mode {
    H2_LOADER_BLE_ADVERTISING_EXTENDED = 0,
    H2_LOADER_BLE_ADVERTISING_LEGACY = 1,
} h2_loader_ble_advertising_mode_t;

extern const uint8_t h2_loader_ble_service_uuid_bytes[16];
extern const uint8_t h2_loader_ble_tx_uuid_bytes[16];
extern const uint8_t h2_loader_ble_rx_uuid_bytes[16];

/** Encode the versioned Service Data payload, excluding its 128-bit UUID. */
int h2_loader_ble_encode_identity(
    uint32_t capabilities,
    const char *board,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_len);

typedef struct h2_loader_ble_service h2_loader_ble_service_t;

typedef struct h2_loader_ble_service_config {
    h2_bleikcp_api_t api;
    const char *board;
    uint32_t capabilities;
    h2_loader_ble_advertising_mode_t advertising_mode;
    h2_bleikcp_server_handler_fn handler;
    void *handler_user;
} h2_loader_ble_service_config_t;

int h2_loader_ble_service_open(
    const h2_loader_ble_service_config_t *config,
    h2_loader_ble_service_t **out_service);

/** Idempotently stop connectable advertising without closing GATT or iKCP. */
int h2_loader_ble_service_pause_advertising(
    h2_loader_ble_service_t *service);

/** Idempotently resume the current connectable advertising payload. */
int h2_loader_ble_service_resume_advertising(
    h2_loader_ble_service_t *service);

/**
 * Copy up to three service UUIDs into the advertising payload alongside the
 * H2Loader management UUID. An active connection defers the payload update
 * until disconnect; the caller retains ownership of the input array.
 */
int h2_loader_ble_service_set_additional_advertised_services(
    h2_loader_ble_service_t *service,
    const h2_pal_ble_uuid_t *services,
    size_t service_count);
int h2_loader_ble_service_close(h2_loader_ble_service_t *service);

/** Returns a borrowed synchronous command I/O adapter for one BLE iKCP session. */
h2_command_io_api_t h2_loader_ble_command_io(h2_bleikcp_t *stream);

/** App-console adapters. read_byte returns SESSION_CLOSED after disconnect. */
int h2_loader_ble_app_read_byte(void *user, uint32_t timeout_ms);
int h2_loader_ble_app_write(void *user, const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
