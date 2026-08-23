#include "h2_quectel_internal.h"

#include <string.h>

h2_pal_result_t h2_quectel_modem_get_capabilities(
    h2_pal_modem_t *platform,
    uint32_t *out_capabilities);
h2_pal_result_t h2_quectel_modem_get_status(
    h2_pal_modem_t *platform,
    h2_pal_modem_status_t *out_status);
h2_pal_result_t h2_quectel_modem_get_identity(
    h2_pal_modem_t *platform,
    h2_pal_modem_identity_t *out_identity);
h2_pal_result_t h2_quectel_modem_get_operator(
    h2_pal_modem_t *platform,
    h2_pal_modem_operator_t *out_operator);
h2_pal_result_t h2_quectel_modem_set_apn(
    h2_pal_modem_t *platform,
    const h2_pal_modem_apn_config_t *config);
h2_pal_result_t h2_quectel_modem_open(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_quectel_modem_close(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_quectel_modem_data_open(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_quectel_modem_data_close(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_quectel_modem_get_data_status(
    h2_pal_modem_t *platform,
    h2_pal_modem_data_status_t *out_status);
h2_pal_result_t h2_quectel_modem_get_signal(
    h2_pal_modem_t *platform,
    h2_pal_modem_signal_t *out_signal);
h2_pal_result_t h2_quectel_modem_call_dial(
    h2_pal_modem_t *platform,
    const h2_pal_modem_call_request_t *request);
h2_pal_result_t h2_quectel_modem_call_answer(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_quectel_modem_call_hangup(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_quectel_modem_get_call_status(
    h2_pal_modem_t *platform,
    h2_pal_modem_call_status_t *out_status);
h2_pal_result_t h2_quectel_modem_gnss_start(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_quectel_modem_gnss_stop(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_quectel_modem_get_gnss_state(
    h2_pal_modem_t *platform,
    h2_pal_modem_gnss_state_t *out_state);
h2_pal_result_t h2_quectel_modem_get_gnss_fix(
    h2_pal_modem_t *platform,
    h2_pal_modem_gnss_fix_t *out_fix);

static h2_pal_modem_t *quectel_platform_from_user(void *user) {
    h2_quectel_modem_t *modem = (h2_quectel_modem_t *)user;
    return modem != NULL ? &modem->platform : NULL;
}

static h2_pal_result_t quectel_open(void *user, uint32_t timeout_ms) {
    return h2_quectel_modem_open(quectel_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t quectel_close(void *user, uint32_t timeout_ms) {
    return h2_quectel_modem_close(quectel_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t quectel_get_capabilities(void *user, uint32_t *out_capabilities) {
    return h2_quectel_modem_get_capabilities(quectel_platform_from_user(user), out_capabilities);
}

static h2_pal_result_t quectel_get_status(void *user, h2_pal_modem_status_t *out_status) {
    return h2_quectel_modem_get_status(quectel_platform_from_user(user), out_status);
}

static h2_pal_result_t quectel_get_identity(void *user, h2_pal_modem_identity_t *out_identity) {
    return h2_quectel_modem_get_identity(quectel_platform_from_user(user), out_identity);
}

static h2_pal_result_t quectel_get_operator(void *user, h2_pal_modem_operator_t *out_operator) {
    return h2_quectel_modem_get_operator(quectel_platform_from_user(user), out_operator);
}

static h2_pal_result_t quectel_set_apn(void *user, const h2_pal_modem_apn_config_t *config) {
    return h2_quectel_modem_set_apn(quectel_platform_from_user(user), config);
}

static h2_pal_result_t quectel_data_open(void *user, uint32_t timeout_ms) {
    return h2_quectel_modem_data_open(quectel_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t quectel_data_close(void *user, uint32_t timeout_ms) {
    return h2_quectel_modem_data_close(quectel_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t quectel_get_data_status(void *user, h2_pal_modem_data_status_t *out_status) {
    return h2_quectel_modem_get_data_status(quectel_platform_from_user(user), out_status);
}

static h2_pal_result_t quectel_get_signal(void *user, h2_pal_modem_signal_t *out_signal) {
    return h2_quectel_modem_get_signal(quectel_platform_from_user(user), out_signal);
}

static h2_pal_result_t quectel_call_dial(void *user, const h2_pal_modem_call_request_t *request) {
    return h2_quectel_modem_call_dial(quectel_platform_from_user(user), request);
}

static h2_pal_result_t quectel_call_answer(void *user, uint32_t timeout_ms) {
    return h2_quectel_modem_call_answer(quectel_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t quectel_call_hangup(void *user, uint32_t timeout_ms) {
    return h2_quectel_modem_call_hangup(quectel_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t quectel_get_call_status(void *user, h2_pal_modem_call_status_t *out_status) {
    return h2_quectel_modem_get_call_status(quectel_platform_from_user(user), out_status);
}

static h2_pal_result_t quectel_gnss_start(void *user, uint32_t timeout_ms) {
    return h2_quectel_modem_gnss_start(quectel_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t quectel_gnss_stop(void *user, uint32_t timeout_ms) {
    return h2_quectel_modem_gnss_stop(quectel_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t quectel_get_gnss_state(void *user, h2_pal_modem_gnss_state_t *out_state) {
    return h2_quectel_modem_get_gnss_state(quectel_platform_from_user(user), out_state);
}

static h2_pal_result_t quectel_get_gnss_fix(void *user, h2_pal_modem_gnss_fix_t *out_fix) {
    return h2_quectel_modem_get_gnss_fix(quectel_platform_from_user(user), out_fix);
}

static const h2_pal_modem_vtable_t s_quectel_modem_vtable = {
    .open = quectel_open,
    .close = quectel_close,
    .get_capabilities = quectel_get_capabilities,
    .get_status = quectel_get_status,
    .get_identity = quectel_get_identity,
    .get_operator = quectel_get_operator,
    .set_apn = quectel_set_apn,
    .data_open = quectel_data_open,
    .data_close = quectel_data_close,
    .get_data_status = quectel_get_data_status,
    .get_signal = quectel_get_signal,
    .call_dial = quectel_call_dial,
    .call_answer = quectel_call_answer,
    .call_hangup = quectel_call_hangup,
    .get_call_status = quectel_get_call_status,
    .gnss_start = quectel_gnss_start,
    .gnss_stop = quectel_gnss_stop,
    .get_gnss_state = quectel_get_gnss_state,
    .get_gnss_fix = quectel_get_gnss_fix,
};

h2_pal_result_t h2_quectel_modem_open(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->opened != 0u) {
        return H2_PAL_OK;
    }
    if (modem->config.init != NULL) {
        h2_pal_result_t rc = modem->config.init(modem->config.transport_user);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    h2_pal_result_t rc = h2_quectel_modem_prepare(modem);
    if (rc != H2_PAL_OK && modem->config.deinit != NULL) {
        (void)modem->config.deinit(modem->config.transport_user);
    }
    if (rc == H2_PAL_OK) {
        modem->opened = 1u;
    }
    return rc;
}

h2_pal_result_t h2_quectel_modem_close(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->opened == 0u) {
        return H2_PAL_OK;
    }
    (void)h2_quectel_modem_drop_ppp(modem);
    (void)h2_quectel_incoming_call_end(modem);
    modem->opened = 0u;
    modem->prepared = 0u;
    if (modem->config.deinit != NULL) {
        return modem->config.deinit(modem->config.transport_user);
    }
    return H2_PAL_OK;
}

h2_quectel_modem_t *h2_quectel_from_platform(h2_pal_modem_t *platform) {
    return platform != NULL ? (h2_quectel_modem_t *)platform->user : NULL;
}

h2_pal_result_t h2_quectel_modem_prepare(h2_quectel_modem_t *modem);

uint32_t h2_quectel_modem_capabilities(const h2_quectel_modem_t *modem) {
    return modem != NULL ? modem->capabilities : 0u;
}

void h2_quectel_post_system_event(
    h2_quectel_modem_t *modem,
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payload_size) {
    if (modem == NULL || modem->config.system_events == NULL) {
        return;
    }
    h2_pal_system_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.payload = payload;
    event.payload_size = payload != NULL ? payload_size : 0u;
    (void)h2_pal_system_event_post(modem->config.system_events, &event, 0u);
}

h2_pal_result_t h2_quectel_modem_init(
    h2_quectel_modem_t *modem,
    const h2_quectel_modem_config_t *config) {
    if (modem == NULL ||
        config == NULL ||
        (config->command == NULL && (config->read == NULL || config->write == NULL))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(modem, 0, sizeof(*modem));
    modem->config = *config;
    if (modem->config.command_timeout_ms == 0u) {
        modem->config.command_timeout_ms = 5000u;
    }
    if (modem->config.io_timeout_ms == 0u) {
        modem->config.io_timeout_ms = 200u;
    }
    modem->capabilities = config->capabilities != 0u
        ? config->capabilities
        : (H2_PAL_MODEM_CAPABILITY_CALL | H2_PAL_MODEM_CAPABILITY_GNSS);
    if (config->sync_api != NULL) {
        h2_pal_mutex_config_t mutex_config = {
            .name = "quectel/at",
            .allocator = config->allocator,
            .flags = 0u,
        };
        h2_pal_result_t rc = h2_pal_mutex_create(config->sync_api, &mutex_config, &modem->lock);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    modem->platform.user = modem;
    modem->platform.vtable = &s_quectel_modem_vtable;
    modem->data_status.state = H2_PAL_MODEM_DATA_CLOSED;
    return H2_PAL_OK;
}

void h2_quectel_modem_deinit(h2_quectel_modem_t *modem) {
    if (modem == NULL) {
        return;
    }
    if (modem->opened != 0u && modem->config.deinit != NULL) {
        (void)modem->config.deinit(modem->config.transport_user);
    }
    if (modem->lock != NULL && modem->config.sync_api != NULL) {
        h2_pal_mutex_destroy(modem->config.sync_api, modem->lock);
    }
    memset(modem, 0, sizeof(*modem));
}

h2_pal_modem_t *h2_quectel_modem_platform(h2_quectel_modem_t *modem) {
    return modem != NULL ? &modem->platform : NULL;
}
