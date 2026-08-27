#ifndef H2_SIMCOM_MODEM_H
#define H2_SIMCOM_MODEM_H

#include "h2/pal/hal/h2_pal_modem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_system_event.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_SIMCOM_LINE_MAX 192u
#define H2_SIMCOM_RESPONSE_MAX 4u

typedef struct h2_simcom_modem h2_simcom_modem_t;

typedef h2_pal_result_t (*h2_simcom_modem_init_fn)(void *user);
/**
 * @brief Power off the whole modem and release its transport resources.
 *
 * This callback must not depend on a graceful data-session close. On failure,
 * it must leave the transport safe to retry.
 */
typedef h2_pal_result_t (*h2_simcom_modem_deinit_fn)(void *user);
typedef h2_pal_result_t (*h2_simcom_modem_flush_fn)(void *user);
typedef h2_pal_result_t (*h2_simcom_modem_read_fn)(
    void *user,
    uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *out_len);
typedef h2_pal_result_t (*h2_simcom_modem_write_fn)(
    void *user,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *out_len);
typedef h2_pal_result_t (*h2_simcom_modem_command_fn)(
    void *user,
    const char *cmd,
    char *response,
    size_t response_size,
    uint32_t timeout_ms);
typedef h2_pal_result_t (*h2_simcom_modem_data_open_fn)(
    void *user,
    uint32_t timeout_ms,
    h2_pal_modem_data_status_t *out_status);
typedef h2_pal_result_t (*h2_simcom_modem_data_close_fn)(
    void *user,
    uint32_t timeout_ms);
typedef h2_pal_result_t (*h2_simcom_modem_wait_gnss_ready_fn)(
    void *user,
    uint32_t timeout_ms);

typedef struct h2_simcom_modem_config {
    void *transport_user;
    h2_simcom_modem_init_fn init;
    h2_simcom_modem_deinit_fn deinit;
    h2_simcom_modem_flush_fn flush;
    h2_simcom_modem_read_fn read;
    h2_simcom_modem_write_fn write;
    h2_simcom_modem_command_fn command;
    h2_simcom_modem_data_open_fn data_open;
    h2_simcom_modem_data_close_fn data_close;
    h2_simcom_modem_wait_gnss_ready_fn wait_gnss_ready;
    const h2_pal_sync_api_t *sync_api;
    const h2_pal_mem_api_t *allocator;
    const h2_pal_system_event_api_t *system_events;
    uint32_t capabilities;
    uint32_t command_timeout_ms;
    uint32_t io_timeout_ms;
} h2_simcom_modem_config_t;

struct h2_simcom_modem {
    h2_pal_modem_t platform;
    h2_simcom_modem_config_t config;
    h2_pal_mutex_t *lock;
    uint8_t prepared;
    uint8_t opened;
    uint32_t configured_capabilities;
    uint32_t capabilities;
    uint8_t gnss_ready_observed;
    h2_pal_modem_data_status_t data_status;
    char last_apn[H2_PAL_MODEM_APN_MAX];
    char last_username[H2_PAL_MODEM_APN_MAX];
    char last_password[H2_PAL_MODEM_APN_MAX];
};

h2_pal_result_t h2_simcom_modem_init(
    h2_simcom_modem_t *modem,
    const h2_simcom_modem_config_t *config);
void h2_simcom_modem_deinit(h2_simcom_modem_t *modem);
h2_pal_modem_t *h2_simcom_modem_platform(h2_simcom_modem_t *modem);
h2_pal_result_t h2_simcom_modem_set_apn(
    h2_pal_modem_t *platform,
    const h2_pal_modem_apn_config_t *config);
h2_pal_result_t h2_simcom_modem_prepare(h2_simcom_modem_t *modem);
void h2_simcom_handle_urc_line(h2_simcom_modem_t *modem, const char *line);
h2_pal_result_t h2_simcom_modem_dial_ppp(h2_simcom_modem_t *modem);
h2_pal_result_t h2_simcom_modem_drop_ppp(h2_simcom_modem_t *modem);
void h2_simcom_modem_notify_data_closed(
    h2_simcom_modem_t *modem,
    h2_pal_result_t last_error);

#ifdef __cplusplus
}
#endif

#endif
