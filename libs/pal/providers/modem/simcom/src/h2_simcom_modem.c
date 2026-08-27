#include "h2_simcom_internal.h"

#include <string.h>

h2_pal_result_t h2_simcom_modem_get_capabilities(
    h2_pal_modem_t *platform,
    uint32_t *out_capabilities);
h2_pal_result_t h2_simcom_modem_get_status(
    h2_pal_modem_t *platform,
    h2_pal_modem_status_t *out_status);
h2_pal_result_t h2_simcom_modem_get_identity(
    h2_pal_modem_t *platform,
    h2_pal_modem_identity_t *out_identity);
h2_pal_result_t h2_simcom_modem_get_operator(
    h2_pal_modem_t *platform,
    h2_pal_modem_operator_t *out_operator);
h2_pal_result_t h2_simcom_modem_set_apn(
    h2_pal_modem_t *platform,
    const h2_pal_modem_apn_config_t *config);
h2_pal_result_t h2_simcom_modem_open(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_simcom_modem_close(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_simcom_modem_data_open(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_simcom_modem_data_close(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_simcom_modem_get_data_status(
    h2_pal_modem_t *platform,
    h2_pal_modem_data_status_t *out_status);
h2_pal_result_t h2_simcom_modem_get_signal(
    h2_pal_modem_t *platform,
    h2_pal_modem_signal_t *out_signal);
h2_pal_result_t h2_simcom_modem_call_dial(
    h2_pal_modem_t *platform,
    const h2_pal_modem_call_request_t *request);
h2_pal_result_t h2_simcom_modem_call_answer(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_simcom_modem_call_hangup(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_simcom_modem_get_call_status(
    h2_pal_modem_t *platform,
    h2_pal_modem_call_status_t *out_status);
h2_pal_result_t h2_simcom_modem_gnss_start(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_simcom_modem_gnss_stop(h2_pal_modem_t *platform, uint32_t timeout_ms);
h2_pal_result_t h2_simcom_modem_get_gnss_state(
    h2_pal_modem_t *platform,
    h2_pal_modem_gnss_state_t *out_state);
h2_pal_result_t h2_simcom_modem_get_gnss_fix(
    h2_pal_modem_t *platform,
    h2_pal_modem_gnss_fix_t *out_fix);

static h2_pal_modem_t *simcom_platform_from_user(void *user) {
    h2_simcom_modem_t *modem = (h2_simcom_modem_t *)user;
    return modem != NULL ? &modem->platform : NULL;
}

static h2_pal_result_t simcom_open(void *user, uint32_t timeout_ms) {
    return h2_simcom_modem_open(simcom_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t simcom_close(void *user, uint32_t timeout_ms) {
    return h2_simcom_modem_close(simcom_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t simcom_get_capabilities(void *user, uint32_t *out_capabilities) {
    return h2_simcom_modem_get_capabilities(simcom_platform_from_user(user), out_capabilities);
}

static h2_pal_result_t simcom_get_status(void *user, h2_pal_modem_status_t *out_status) {
    return h2_simcom_modem_get_status(simcom_platform_from_user(user), out_status);
}

static h2_pal_result_t simcom_get_identity(void *user, h2_pal_modem_identity_t *out_identity) {
    return h2_simcom_modem_get_identity(simcom_platform_from_user(user), out_identity);
}

static h2_pal_result_t simcom_get_operator(void *user, h2_pal_modem_operator_t *out_operator) {
    return h2_simcom_modem_get_operator(simcom_platform_from_user(user), out_operator);
}

static h2_pal_result_t simcom_set_apn(void *user, const h2_pal_modem_apn_config_t *config) {
    return h2_simcom_modem_set_apn(simcom_platform_from_user(user), config);
}

static h2_pal_result_t simcom_data_open(void *user, uint32_t timeout_ms) {
    return h2_simcom_modem_data_open(simcom_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t simcom_data_close(void *user, uint32_t timeout_ms) {
    return h2_simcom_modem_data_close(simcom_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t simcom_get_data_status(void *user, h2_pal_modem_data_status_t *out_status) {
    return h2_simcom_modem_get_data_status(simcom_platform_from_user(user), out_status);
}

static h2_pal_result_t simcom_get_signal(void *user, h2_pal_modem_signal_t *out_signal) {
    return h2_simcom_modem_get_signal(simcom_platform_from_user(user), out_signal);
}

static h2_pal_result_t simcom_call_dial(void *user, const h2_pal_modem_call_request_t *request) {
    return h2_simcom_modem_call_dial(simcom_platform_from_user(user), request);
}

static h2_pal_result_t simcom_call_answer(void *user, uint32_t timeout_ms) {
    return h2_simcom_modem_call_answer(simcom_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t simcom_call_hangup(void *user, uint32_t timeout_ms) {
    return h2_simcom_modem_call_hangup(simcom_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t simcom_get_call_status(void *user, h2_pal_modem_call_status_t *out_status) {
    return h2_simcom_modem_get_call_status(simcom_platform_from_user(user), out_status);
}

static h2_pal_result_t simcom_gnss_start(void *user, uint32_t timeout_ms) {
    return h2_simcom_modem_gnss_start(simcom_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t simcom_gnss_stop(void *user, uint32_t timeout_ms) {
    return h2_simcom_modem_gnss_stop(simcom_platform_from_user(user), timeout_ms);
}

static h2_pal_result_t simcom_get_gnss_state(void *user, h2_pal_modem_gnss_state_t *out_state) {
    return h2_simcom_modem_get_gnss_state(simcom_platform_from_user(user), out_state);
}

static h2_pal_result_t simcom_get_gnss_fix(void *user, h2_pal_modem_gnss_fix_t *out_fix) {
    return h2_simcom_modem_get_gnss_fix(simcom_platform_from_user(user), out_fix);
}

static const h2_pal_modem_vtable_t s_simcom_modem_vtable = {
    .open = simcom_open,
    .close = simcom_close,
    .get_capabilities = simcom_get_capabilities,
    .get_status = simcom_get_status,
    .get_identity = simcom_get_identity,
    .get_operator = simcom_get_operator,
    .set_apn = simcom_set_apn,
    .data_open = simcom_data_open,
    .data_close = simcom_data_close,
    .get_data_status = simcom_get_data_status,
    .get_signal = simcom_get_signal,
    .call_dial = simcom_call_dial,
    .call_answer = simcom_call_answer,
    .call_hangup = simcom_call_hangup,
    .get_call_status = simcom_get_call_status,
    .gnss_start = simcom_gnss_start,
    .gnss_stop = simcom_gnss_stop,
    .get_gnss_state = simcom_get_gnss_state,
    .get_gnss_fix = simcom_get_gnss_fix,
};

static unsigned char ascii_upper(unsigned char value) {
    if (value >= (unsigned char)'a' && value <= (unsigned char)'z') {
        return (unsigned char)(value - (unsigned char)'a' + (unsigned char)'A');
    }
    return value;
}

static int contains_case_insensitive(const char *text, const char *needle) {
    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return 0;
    }
    const size_t needle_len = strlen(needle);
    for (const char *start = text; *start != '\0'; ++start) {
        size_t i = 0u;
        while (i < needle_len && start[i] != '\0' &&
               ascii_upper((unsigned char)start[i]) ==
                   ascii_upper((unsigned char)needle[i])) {
            ++i;
        }
        if (i == needle_len) {
            return 1;
        }
    }
    return 0;
}

static uint32_t probe_capabilities(h2_simcom_modem_t *modem) {
    uint32_t capabilities = 0u;

    if ((modem->configured_capabilities & H2_PAL_MODEM_CAPABILITY_DATA) != 0u &&
        modem->config.data_open != NULL && modem->config.data_close != NULL &&
        h2_simcom_at_exchange(modem, "AT+CGATT?", NULL, 0) == H2_PAL_OK) {
        capabilities |= H2_PAL_MODEM_CAPABILITY_DATA;
    }
    if ((modem->configured_capabilities & H2_PAL_MODEM_CAPABILITY_CALL) != 0u &&
        h2_simcom_at_exchange(modem, "AT+CLCC", NULL, 0) == H2_PAL_OK) {
        capabilities |= H2_PAL_MODEM_CAPABILITY_CALL;
    }
    if ((modem->configured_capabilities & H2_PAL_MODEM_CAPABILITY_GNSS) != 0u &&
        h2_simcom_at_exchange(modem, "AT+CGNSSPWR?", NULL, 0) == H2_PAL_OK) {
        capabilities |= H2_PAL_MODEM_CAPABILITY_GNSS;
    }
    return capabilities;
}

h2_pal_result_t h2_simcom_modem_open(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
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
    h2_pal_result_t rc = h2_simcom_modem_prepare(modem);
    if (rc == H2_PAL_OK) {
        h2_pal_modem_identity_t identity;
        rc = h2_simcom_modem_get_identity(platform, &identity);
        if (rc == H2_PAL_OK &&
            (!contains_case_insensitive(identity.model, "A7670") ||
             (!contains_case_insensitive(identity.manufacturer, "SIMCOM") &&
              !contains_case_insensitive(identity.manufacturer, "INCORPORATED")))) {
            rc = H2_PAL_ERR_UNSUPPORTED;
        }
    }
    if (rc == H2_PAL_OK) {
        modem->capabilities = probe_capabilities(modem);
        modem->opened = 1u;
    } else {
        modem->prepared = 0u;
        modem->capabilities = 0u;
        if (modem->config.deinit != NULL) {
            (void)modem->config.deinit(modem->config.transport_user);
        }
    }
    return rc;
}

h2_pal_result_t h2_simcom_modem_close(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->opened == 0u) {
        return H2_PAL_OK;
    }
    if (modem->config.deinit != NULL) {
        const h2_pal_result_t rc = modem->config.deinit(modem->config.transport_user);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    modem->opened = 0u;
    modem->prepared = 0u;
    modem->capabilities = 0u;
    h2_simcom_modem_notify_data_closed(modem, H2_PAL_OK);
    return H2_PAL_OK;
}

h2_simcom_modem_t *h2_simcom_from_platform(h2_pal_modem_t *platform) {
    return platform != NULL ? (h2_simcom_modem_t *)platform->user : NULL;
}

h2_pal_result_t h2_simcom_modem_prepare(h2_simcom_modem_t *modem);

uint32_t h2_simcom_modem_capabilities(const h2_simcom_modem_t *modem) {
    return modem != NULL ? modem->capabilities : 0u;
}

void h2_simcom_post_system_event(
    h2_simcom_modem_t *modem,
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

h2_pal_result_t h2_simcom_modem_init(
    h2_simcom_modem_t *modem,
    const h2_simcom_modem_config_t *config) {
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
    const uint32_t implemented_capabilities =
        H2_PAL_MODEM_CAPABILITY_CALL |
        H2_PAL_MODEM_CAPABILITY_GNSS |
        (config->data_open != NULL && config->data_close != NULL
            ? H2_PAL_MODEM_CAPABILITY_DATA
            : 0u);
    modem->configured_capabilities = (config->capabilities != 0u
        ? config->capabilities
        : implemented_capabilities) & implemented_capabilities;
    if (config->sync_api != NULL) {
        h2_pal_mutex_config_t mutex_config = {
            .name = "simcom/at",
            .allocator = config->allocator,
            .flags = 0u,
        };
        h2_pal_result_t rc = h2_pal_mutex_create(config->sync_api, &mutex_config, &modem->lock);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    modem->platform.user = modem;
    modem->platform.vtable = &s_simcom_modem_vtable;
    modem->data_status.state = H2_PAL_MODEM_DATA_CLOSED;
    return H2_PAL_OK;
}

void h2_simcom_modem_deinit(h2_simcom_modem_t *modem) {
    if (modem == NULL) {
        return;
    }
    if (modem->opened != 0u) {
        (void)h2_simcom_modem_close(&modem->platform, modem->config.command_timeout_ms);
    }
    if (modem->lock != NULL && modem->config.sync_api != NULL) {
        h2_pal_mutex_destroy(modem->config.sync_api, modem->lock);
    }
    memset(modem, 0, sizeof(*modem));
}

h2_pal_modem_t *h2_simcom_modem_platform(h2_simcom_modem_t *modem) {
    return modem != NULL ? &modem->platform : NULL;
}
