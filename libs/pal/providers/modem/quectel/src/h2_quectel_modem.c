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

static h2_pal_result_t quectel_cell_locate(
    void *user,
    uint32_t timeout_ms,
    h2_pal_modem_cell_location_t *out_location) {
    return h2_quectel_modem_cell_locate(quectel_platform_from_user(user), timeout_ms, out_location);
}

/* Cell locate is the only operation the provider leaves out of the dispatch
 * table when it is not configured, so the PAL wrapper answers UNSUPPORTED
 * without the provider having to hold an unusable token. */
#define H2_QUECTEL_MODEM_VTABLE_BASE \
    .set_power_policy = h2_quectel_set_power_policy, \
    .get_power_status = h2_quectel_get_power_status, \
    .open = quectel_open, \
    .close = quectel_close, \
    .get_capabilities = quectel_get_capabilities, \
    .get_status = quectel_get_status, \
    .get_identity = quectel_get_identity, \
    .get_operator = quectel_get_operator, \
    .set_apn = quectel_set_apn, \
    .data_open = quectel_data_open, \
    .data_close = quectel_data_close, \
    .get_data_status = quectel_get_data_status, \
    .get_signal = quectel_get_signal, \
    .call_dial = quectel_call_dial, \
    .call_answer = quectel_call_answer, \
    .call_hangup = quectel_call_hangup, \
    .get_call_status = quectel_get_call_status, \
    .gnss_start = quectel_gnss_start, \
    .gnss_stop = quectel_gnss_stop, \
    .get_gnss_state = quectel_get_gnss_state, \
    .get_gnss_fix = quectel_get_gnss_fix

static const h2_pal_modem_vtable_t s_quectel_modem_vtable = {
    H2_QUECTEL_MODEM_VTABLE_BASE,
};

static const h2_pal_modem_vtable_t s_quectel_modem_cell_locate_vtable = {
    H2_QUECTEL_MODEM_VTABLE_BASE,
    .cell_locate = quectel_cell_locate,
};

static h2_pal_result_t h2_quectel_modem_open_impl(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->opened != 0u) {
        return H2_PAL_OK;
    }
    if (modem->config.init != NULL) {
        h2_quectel_state_unlock(modem);
        h2_pal_result_t rc = modem->config.init(modem->config.transport_user);
        (void)h2_quectel_state_lock(modem);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    h2_pal_result_t rc = h2_quectel_modem_prepare(modem);
    if (rc != H2_PAL_OK && modem->config.deinit != NULL) {
        h2_quectel_state_unlock(modem);
        (void)modem->config.deinit(modem->config.transport_user);
        (void)h2_quectel_state_lock(modem);
    }
    if (rc == H2_PAL_OK) {
        modem->opened = 1u;
    }
    return rc;
}

static h2_pal_result_t h2_quectel_modem_close_impl(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->opened == 0u) {
        return H2_PAL_OK;
    }
    h2_pal_result_t result = h2_quectel_power_wake(modem);
    if (modem->gnss_hold != 0u) {
        h2_pal_result_t rc = h2_quectel_modem_gnss_stop(platform, timeout_ms);
        if (result == H2_PAL_OK) { result = rc; }
    }
    if (modem->call_hold != 0u) {
        h2_pal_result_t rc = h2_quectel_modem_call_hangup(platform, timeout_ms);
        if (result == H2_PAL_OK) { result = rc; }
    }
    h2_pal_result_t rc = h2_quectel_modem_drop_ppp(modem);
    if (result == H2_PAL_OK) { result = rc; }
    if ((modem->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u) {
        rc = h2_quectel_at_exchange(modem, "AT+QSCLK=0", NULL, 0);
        if (result == H2_PAL_OK) { result = rc; }
    }
    /* A failed stop leaves the modem session unconfirmed. Keep transport,
     * state and lock alive so close/deinit can retry that session. */
    if (result != H2_PAL_OK) {
        modem->power_fault = 1u;
        return result;
    }
    if (modem->config.deinit != NULL) {
        h2_quectel_state_unlock(modem);
        rc = modem->config.deinit(modem->config.transport_user);
        (void)h2_quectel_state_lock(modem);
        if (rc != H2_PAL_OK) {
            modem->power_fault = 1u;
            return rc;
        }
    }
    (void)h2_quectel_incoming_call_end(modem);
    modem->power_policy = H2_PAL_MODEM_POWER_POLICY_ACTIVE;
    modem->power_configured = 0u;
    modem->power_fault = 0u;
    modem->sleep_allowed = 0u;
    modem->gnss_hold = 0u;
    modem->call_hold = 0u;
    modem->data_hold = 0u;
    modem->model_checked = 0u;
    modem->sim_seen = 0u;
    modem->registration_seen = 0u;
    modem->packet_seen = 0u;
    modem->signal_seen = 0u;
    modem->sim_state = H2_PAL_MODEM_SIM_STATE_UNKNOWN;
    memset(&modem->data_status, 0, sizeof(modem->data_status));
    modem->data_status.state = H2_PAL_MODEM_DATA_CLOSED;
    if (modem->config.invalidate_data != NULL) {
        modem->config.invalidate_data(modem->config.transport_user);
    }
    modem->opened = 0u;
    modem->prepared = 0u;
    /* The modem keeps the token only while it stays powered through this
     * instance, so the next open has to configure it again. */
    modem->cell_locate_token_sent = 0u;
    return result;
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
    if (modem == NULL) {
        return;
    }
    /* All producers hold the state lock. Compare semantic fields rather than
     * struct padding or URC strings, preserving A -> B -> A transitions. */
    if (payload != NULL && payload_size == sizeof(h2_pal_modem_status_t)) {
        const h2_pal_modem_status_t *status = payload;
        if (type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_REGISTRATION_CHANGED) {
            if (modem->registration_seen && modem->observed_status.registration == status->registration) {
                return;
            }
            modem->registration_seen = 1u;
            modem->registration_generation++;
            modem->observed_status.registration = status->registration;
        } else if (type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_PACKET_CHANGED) {
            if (modem->packet_seen && modem->observed_status.packet == status->packet) {
                return;
            }
            modem->packet_seen = 1u;
            modem->packet_generation++;
            modem->observed_status.packet = status->packet;
        }
    }
    if (type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_SIGNAL_CHANGED && payload != NULL &&
        payload_size == sizeof(h2_pal_modem_signal_t)) {
        const h2_pal_modem_signal_t *signal = payload;
        if (modem->signal_seen && modem->observed_signal.rssi_dbm == signal->rssi_dbm &&
            modem->observed_signal.ber == signal->ber && modem->observed_signal.rat == signal->rat) {
            return;
        }
        modem->signal_seen = 1u;
        modem->observed_signal = *signal;
    }
    h2_pal_system_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.payload = payload;
    event.payload_size = payload != NULL ? payload_size : 0u;
    if (modem->config.system_events != NULL &&
        h2_pal_system_event_post(modem->config.system_events, &event, 0u) != H2_PAL_OK) {
        modem->event_drop_count++;
    }
}

static void dispatch_urc(void *user, const char *line) {
    h2_quectel_modem_t *modem = user;
    h2_quectel_handle_urc_line(modem, line);
}

h2_pal_result_t h2_quectel_post_urc_line(h2_quectel_modem_t *modem, const char *line) {
    if (modem == NULL || line == NULL) { return H2_PAL_ERR_INVALID_ARG; }
    return h2_quectel_is_urc(line, NULL)
        ? h2_modem_urc_post(&modem->urc_worker, line) : H2_PAL_OK;
}

h2_pal_result_t h2_quectel_modem_init(
    h2_quectel_modem_t *modem,
    const h2_quectel_modem_config_t *config) {
    if (modem == NULL ||
        config == NULL ||
        (config->command == NULL && (config->read == NULL || config->write == NULL))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if ((config->urc_task_api != NULL || config->urc_queue_api != NULL) &&
        (config->urc_task_api == NULL || config->urc_queue_api == NULL || config->sync_api == NULL)) {
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
    modem->capabilities &= ~(uint32_t)H2_PAL_MODEM_CAPABILITY_LOW_POWER;
    if (config->profile == H2_QUECTEL_MODEM_PROFILE_EC25_UART &&
        config->sleep_gate != NULL && config->sync_api != NULL && config->command != NULL) {
        modem->capabilities |= H2_PAL_MODEM_CAPABILITY_LOW_POWER;
    }
    if (config->sim_hotplug != 0u &&
        (config->profile != H2_QUECTEL_MODEM_PROFILE_EC25_UART ||
         config->sync_api == NULL || config->command == NULL || config->invalidate_data == NULL)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (config->sim_insert_level > 1u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const int cell_locate_ready = h2_quectel_cell_locate_token_valid(config->cell_locate_token);
    if (config->cell_locate_token != NULL &&
        config->cell_locate_token[0] != '\0' &&
        !cell_locate_ready) {
        /* Reject a token that cannot be sent verbatim instead of silently
         * truncating it. The value itself is never reported. */
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (cell_locate_ready) {
        modem->capabilities |= H2_PAL_MODEM_CAPABILITY_CELL_LOCATE;
    } else {
        modem->capabilities &= ~(uint32_t)H2_PAL_MODEM_CAPABILITY_CELL_LOCATE;
    }
    if (config->sync_api != NULL) {
        h2_pal_mutex_config_t mutex_config = {
            .name = "quectel/at",
            .allocator = config->allocator,
            .flags = H2_PAL_MUTEX_FLAG_RECURSIVE,
        };
        h2_pal_result_t rc = h2_pal_mutex_create(config->sync_api, &mutex_config, &modem->lock);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        mutex_config.name = "quectel/operation";
        rc = h2_pal_mutex_create(config->sync_api, &mutex_config, &modem->operation_lock);
        if (rc != H2_PAL_OK) {
            (void)h2_pal_mutex_destroy(config->sync_api, modem->lock);
            modem->lock = NULL;
            return rc;
        }
    }
    modem->platform.user = modem;
    modem->platform.vtable = cell_locate_ready
        ? &s_quectel_modem_cell_locate_vtable
        : &s_quectel_modem_vtable;
    modem->data_status.state = H2_PAL_MODEM_DATA_CLOSED;
    if (config->urc_task_api != NULL) {
        h2_pal_result_t rc = h2_modem_urc_start(&modem->urc_worker,
            config->urc_task_api, config->urc_queue_api, config->allocator, dispatch_urc, modem);
        if (rc != H2_PAL_OK) {
            (void)h2_pal_mutex_destroy(config->sync_api, modem->operation_lock);
            modem->operation_lock = NULL;
            (void)h2_pal_mutex_destroy(config->sync_api, modem->lock);
            modem->lock = NULL;
            return rc;
        }
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_quectel_modem_deinit(h2_quectel_modem_t *modem) {
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->opened != 0u) {
        h2_pal_result_t rc = h2_quectel_modem_close(&modem->platform, modem->config.command_timeout_ms);
        if (rc != H2_PAL_OK && modem->opened != 0u) {
            return rc;
        }
    }
    /* Transport RX is stopped by close (or externally joined by the owner).
     * Never join while holding the provider lock needed by dispatch_urc. */
    h2_pal_result_t rc = h2_modem_urc_stop(&modem->urc_worker);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (modem->lock != NULL && modem->config.sync_api != NULL) {
        rc = h2_pal_mutex_destroy(modem->config.sync_api, modem->lock);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    modem->lock = NULL;
    if (modem->operation_lock != NULL) {
        rc = h2_pal_mutex_destroy(modem->config.sync_api, modem->operation_lock);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    memset(modem, 0, sizeof(*modem));
    return H2_PAL_OK;
}

h2_pal_modem_t *h2_quectel_modem_platform(h2_quectel_modem_t *modem) {
    return modem != NULL ? &modem->platform : NULL;
}

h2_pal_result_t h2_quectel_modem_open(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    h2_quectel_modem_t *modem_state = h2_quectel_from_platform(platform);
    h2_pal_result_t rc = h2_quectel_operation_begin(modem_state);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_quectel_modem_open_impl(platform, timeout_ms);
    return h2_quectel_operation_end(modem_state, rc);
}

h2_pal_result_t h2_quectel_modem_close(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    h2_quectel_modem_t *modem_state = h2_quectel_from_platform(platform);
    h2_pal_result_t rc = h2_quectel_operation_begin(modem_state);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_quectel_modem_close_impl(platform, timeout_ms);
    return h2_quectel_operation_end(modem_state, rc);
}
