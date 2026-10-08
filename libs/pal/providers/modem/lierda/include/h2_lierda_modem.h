#ifndef H2_LIERDA_MODEM_H
#define H2_LIERDA_MODEM_H

#include "h2/pal/hal/h2_pal_modem.h"
#include "h2/pal/os/h2_pal_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_lierda_modem h2_lierda_modem_t;

/** Hardware selection supplied by the board, not inferred from AT text. */
typedef enum h2_lierda_modem_model {
    H2_LIERDA_MODEM_MODEL_NT26KCNB20NNC = 1,
} h2_lierda_modem_model_t;

/** Borrowed transport. Callbacks run under the provider operation mutex and
 * must not reenter its API. SDK/IP callbacks update transport-owned state,
 * never call the provider. All callbacks return errors without logging
 * command responses, APN credentials, IMEI or IMSI.
 * open creates a command-mode transport; close shuts down the whole transport
 * without requiring data_close to succeed. Failures retain retryable ownership.
 * command accepts an unterminated AT command (no CR/LF), consumes final OK/error
 * itself and returns only complete solicited information, NUL terminated, or
 * TRUNCATED. It must not return OK for truncated/ambiguous input.
 * data_open performs the standard Generic DCE dial and waits for real host PPP
 * IP; its APN is borrowed only for the call. On failure it retains data state
 * until data_close/close actually recovers. data_status only reads host PPP
 * state, including after a failed open; it never sends AT or starts a session.
 * Strings and response buffers are borrowed only during each callback. */
typedef struct h2_lierda_modem_transport {
    void *user;
    h2_pal_result_t (*open)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*close)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*command)(
        void *user, const char *command, char *response,
        size_t response_size, uint32_t timeout_ms);
    h2_pal_result_t (*data_open)(
        void *user, const h2_pal_modem_apn_config_t *apn,
        uint32_t timeout_ms);
    h2_pal_result_t (*data_close)(void *user, uint32_t timeout_ms);
    h2_pal_result_t (*data_status)(
        void *user, h2_pal_modem_data_status_t *out_status);
} h2_lierda_modem_transport_t;

typedef struct h2_lierda_modem_config {
    h2_lierda_modem_model_t model;
    h2_lierda_modem_transport_t transport;
    const h2_pal_mem_api_t *allocator; /**< Borrowed through successful destroy. */
    const h2_pal_sync_api_t *sync_api; /**< Borrowed; complete mutex API required. */
    h2_pal_modem_apn_config_t apn; /**< Copied; application selects APN/auth. */
    uint32_t command_timeout_ms; /**< Per command; 0 selects 5000 ms. */
} h2_lierda_modem_config_t;

/** Create a closed, data-only PAL provider; no AT/power operation occurs.
 * Config/APN are copied; transport user and provider APIs stay borrowed.
 * Calls are serialized by one mutex. Lifecycle destruction requires callers
 * already stopped/joined. AT-backed getters and set_apn return BUSY until host
 * data_status confirms CLOSED; identity/model/revision are bounded module text,
 * not a claim that the module reports the selected product SKU. No asynchronous
 * URC/call/GNSS/OTA/CMUX/power-policy capability is exposed.
 * Outputs are cleared on failure. create returns a retained out_modem only if
 * a partially created mutex could not be destroyed; destroy must then be retried.
 * All PAL operations may block in their transport callback; timeout is ms. */
h2_pal_result_t h2_lierda_modem_create(
    const h2_lierda_modem_config_t *config, h2_lierda_modem_t **out_modem);

/** Close transport and destroy mutex. On error keep instance/dependencies
 * alive and retry. Never call from a transport callback or while another caller
 * can be accessing the borrowed PAL object. */
h2_pal_result_t h2_lierda_modem_destroy(h2_lierda_modem_t *modem);

/** Borrowed PAL object valid until successful destroy. open is idempotent;
 * close can recover a failed open and is idempotent after success. */
h2_pal_modem_api_t *h2_lierda_modem_api(h2_lierda_modem_t *modem);

#ifdef __cplusplus
}
#endif
#endif
