#include "h2_lierda_internal.h"

#include <string.h>

static h2_pal_result_t finish(h2_lierda_modem_t *modem, h2_pal_result_t result) {
    const h2_pal_result_t rc = h2_pal_mutex_unlock(
        modem->config.sync_api, modem->mutex);
    return result != H2_PAL_OK ? result : rc;
}

static h2_pal_result_t begin(h2_lierda_modem_t *modem) {
    return h2_pal_mutex_lock(modem->config.sync_api, modem->mutex);
}

h2_pal_result_t h2_lierda_command_guard(h2_lierda_modem_t *modem) {
    if (!modem->opened) return H2_PAL_ERR_CLOSED;
    h2_pal_modem_data_status_t status = {0};
    const h2_pal_result_t rc = modem->config.transport.data_status(
        modem->config.transport.user, &status);
    if (rc != H2_PAL_OK) return rc;
    return status.state == H2_PAL_MODEM_DATA_CLOSED
        ? H2_PAL_OK : H2_PAL_ERR_BUSY;
}

static h2_pal_result_t modem_open(void *user, uint32_t timeout_ms) {
    h2_lierda_modem_t *modem = user;
    h2_pal_result_t rc = begin(modem);
    if (rc != H2_PAL_OK) return rc;
    if (modem->opened) return finish(modem, H2_PAL_OK);
    if (modem->transport_owned) return finish(modem, H2_PAL_ERR_INVALID_STATE);
    modem->transport_owned = true;
    rc = modem->config.transport.open(modem->config.transport.user, timeout_ms);
    char response[H2_LIERDA_RESPONSE_SIZE];
    if (rc == H2_PAL_OK) rc = h2_lierda_command(modem, "AT", response);
    if (rc == H2_PAL_OK) rc = h2_lierda_command(modem, "ATE0", response);
    if (rc == H2_PAL_OK) rc = h2_lierda_command(modem, "AT+CMEE=2", response);
    if (rc == H2_PAL_OK) {
        modem->opened = true;
    } else {
        const h2_pal_result_t close_rc = modem->config.transport.close(
            modem->config.transport.user, timeout_ms);
        if (close_rc == H2_PAL_OK) modem->transport_owned = false;
        else rc = close_rc;
    }
    return finish(modem, rc);
}

static h2_pal_result_t close_locked(h2_lierda_modem_t *modem, uint32_t timeout_ms) {
    if (!modem->transport_owned) return H2_PAL_OK;
    /* A failed partial teardown is not an open command-mode transport. */
    modem->opened = false;
    const h2_pal_result_t rc = modem->config.transport.close(
        modem->config.transport.user, timeout_ms);
    if (rc == H2_PAL_OK) {
        modem->transport_owned = false;
        modem->opened = false;
    }
    return rc;
}

static h2_pal_result_t modem_close(void *user, uint32_t timeout_ms) {
    h2_lierda_modem_t *modem = user;
    h2_pal_result_t rc = begin(modem);
    return rc == H2_PAL_OK ? finish(modem, close_locked(modem, timeout_ms)) : rc;
}

static h2_pal_result_t capabilities(void *user, uint32_t *out_capabilities) {
    (void)user;
    if (out_capabilities == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_capabilities = H2_PAL_MODEM_CAPABILITY_DATA;
    return H2_PAL_OK;
}

#define LIERDA_GETTER(name, type, reader) \
static h2_pal_result_t name(void *user, type *out_value) { \
    if (out_value == NULL) return H2_PAL_ERR_INVALID_ARG; \
    memset(out_value, 0, sizeof(*out_value)); \
    h2_lierda_modem_t *modem = user; \
    h2_pal_result_t rc = begin(modem); \
    if (rc != H2_PAL_OK) return rc; \
    rc = h2_lierda_command_guard(modem); \
    if (rc == H2_PAL_OK) rc = reader(modem, out_value); \
    rc = finish(modem, rc); \
    if (rc != H2_PAL_OK) memset(out_value, 0, sizeof(*out_value)); \
    return rc; \
}

LIERDA_GETTER(get_status, h2_pal_modem_status_t, h2_lierda_read_status)
LIERDA_GETTER(get_identity, h2_pal_modem_identity_t, h2_lierda_read_identity)
LIERDA_GETTER(get_operator, h2_pal_modem_operator_t, h2_lierda_read_operator)
LIERDA_GETTER(get_signal, h2_pal_modem_signal_t, h2_lierda_read_signal)
#undef LIERDA_GETTER

static h2_pal_result_t set_apn(void *user, const h2_pal_modem_apn_config_t *apn) {
    if (!h2_lierda_apn_valid(apn)) return H2_PAL_ERR_INVALID_ARG;
    h2_lierda_modem_t *modem = user;
    h2_pal_result_t rc = begin(modem);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_lierda_command_guard(modem);
    if (rc == H2_PAL_OK) rc = h2_lierda_apply_apn(modem, apn);
    if (rc == H2_PAL_OK) modem->config.apn = *apn;
    return finish(modem, rc);
}

static h2_pal_result_t get_data_status(void *user, h2_pal_modem_data_status_t *out_status) {
    if (out_status == NULL) return H2_PAL_ERR_INVALID_ARG;
    memset(out_status, 0, sizeof(*out_status));
    h2_lierda_modem_t *modem = user;
    h2_pal_result_t rc = begin(modem);
    if (rc != H2_PAL_OK) return rc;
    if (modem->transport_owned) rc = modem->config.transport.data_status(
        modem->config.transport.user, out_status);
    rc = finish(modem, rc);
    if (rc != H2_PAL_OK) memset(out_status, 0, sizeof(*out_status));
    return rc;
}

static h2_pal_result_t data_open(void *user, uint32_t timeout_ms) {
    h2_lierda_modem_t *modem = user;
    h2_pal_result_t rc = begin(modem);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_lierda_command_guard(modem);
    if (rc != H2_PAL_OK) return finish(modem, rc);
    h2_pal_modem_status_t status = {0};
    rc = h2_lierda_read_status(modem, &status);
    if (rc == H2_PAL_OK && (status.sim != H2_PAL_MODEM_SIM_STATE_READY ||
        (status.registration != H2_PAL_MODEM_REGISTRATION_HOME &&
         status.registration != H2_PAL_MODEM_REGISTRATION_ROAMING) ||
        status.packet != H2_PAL_MODEM_PACKET_ATTACHED)) rc = H2_PAL_ERR_INVALID_STATE;
    if (rc == H2_PAL_OK) rc = h2_lierda_apply_apn(modem, &modem->config.apn);
    if (rc == H2_PAL_OK) rc = modem->config.transport.data_open(
        modem->config.transport.user, &modem->config.apn, timeout_ms);
    if (rc == H2_PAL_OK) {
        h2_pal_modem_data_status_t data = {0};
        rc = modem->config.transport.data_status(modem->config.transport.user, &data);
        if (rc == H2_PAL_OK && (data.state != H2_PAL_MODEM_DATA_OPEN ||
            data.ip4_valid == 0u || data.ip4 == 0u)) rc = H2_PAL_ERR_INVALID_STATE;
    }
    return finish(modem, rc);
}

static h2_pal_result_t data_close(void *user, uint32_t timeout_ms) {
    h2_lierda_modem_t *modem = user;
    h2_pal_result_t rc = begin(modem);
    if (rc != H2_PAL_OK) return rc;
    if (modem->transport_owned) rc = modem->config.transport.data_close(
        modem->config.transport.user, timeout_ms);
    return finish(modem, rc);
}

static const h2_pal_modem_vtable_t modem_vtable = {
    .open = modem_open, .close = modem_close,
    .get_capabilities = capabilities, .get_status = get_status,
    .get_identity = get_identity, .get_operator = get_operator,
    .set_apn = set_apn, .data_open = data_open, .data_close = data_close,
    .get_data_status = get_data_status, .get_signal = get_signal,
};

h2_pal_result_t h2_lierda_modem_create(
    const h2_lierda_modem_config_t *config, h2_lierda_modem_t **out_modem) {
    if (out_modem == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_modem = NULL;
    if (config == NULL || config->model != H2_LIERDA_MODEM_MODEL_NT26KCNB20NNC ||
        !h2_lierda_apn_valid(&config->apn) || config->allocator == NULL ||
        config->allocator->vtable == NULL || config->allocator->vtable->alloc == NULL ||
        config->allocator->vtable->free == NULL || config->sync_api == NULL ||
        config->sync_api->vtable == NULL ||
        config->sync_api->vtable->create_mutex == NULL ||
        config->sync_api->vtable->destroy_mutex == NULL ||
        config->sync_api->vtable->lock_mutex == NULL ||
        config->sync_api->vtable->unlock_mutex == NULL ||
        config->transport.open == NULL || config->transport.close == NULL ||
        config->transport.command == NULL || config->transport.data_open == NULL ||
        config->transport.data_close == NULL || config->transport.data_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_lierda_modem_t *modem = h2_pal_mem_alloc(config->allocator, sizeof(*modem));
    if (modem == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(modem, 0, sizeof(*modem));
    modem->config = *config;
    modem->api.user = modem;
    modem->api.vtable = &modem_vtable;
    const h2_pal_mutex_config_t mutex_config = {
        .name = "lierda/operation", .allocator = config->allocator,
    };
    h2_pal_result_t rc = h2_pal_mutex_create(config->sync_api, &mutex_config, &modem->mutex);
    if (rc == H2_PAL_OK && modem->mutex == NULL) rc = H2_PAL_ERR_INVALID_STATE;
    if (rc != H2_PAL_OK) {
        if (modem->mutex != NULL &&
            h2_pal_mutex_destroy(config->sync_api, modem->mutex) != H2_PAL_OK) {
            *out_modem = modem;
        } else h2_pal_mem_free(config->allocator, modem);
        return rc;
    }
    modem->initialized = true;
    *out_modem = modem;
    return H2_PAL_OK;
}

h2_pal_result_t h2_lierda_modem_destroy(h2_lierda_modem_t *modem) {
    if (modem == NULL) return H2_PAL_ERR_INVALID_ARG;
    h2_pal_result_t rc = modem->initialized ? modem_close(modem, 0u) : H2_PAL_OK;
    if (rc != H2_PAL_OK) return rc;
    rc = h2_pal_mutex_destroy(modem->config.sync_api, modem->mutex);
    if (rc != H2_PAL_OK) return rc;
    const h2_pal_mem_api_t *allocator = modem->config.allocator;
    memset(modem, 0, sizeof(*modem));
    h2_pal_mem_free(allocator, modem);
    return H2_PAL_OK;
}

h2_pal_modem_api_t *h2_lierda_modem_api(h2_lierda_modem_t *modem) {
    return modem != NULL && modem->initialized ? &modem->api : NULL;
}
