#include "h2_h2loader_host_internal.h"

#include <string.h>

#define H2_H2LOADER_HOST_DEFAULT_RECONNECT_DELAY_MS 1000u
#define H2_H2LOADER_HOST_DEFAULT_RECONNECT_ATTEMPTS 30u

static void emit_event(
    const h2_h2loader_host_managed_operation_config_t *config,
    h2_h2loader_host_operation_phase_t phase,
    h2_pal_result_t result) {
    if (config->on_event != NULL) {
        config->on_event(config->event_user, phase, result);
    }
}

static int operation_cancelled(
    const h2_h2loader_host_managed_operation_config_t *config) {
    return config->is_cancelled != NULL &&
        config->is_cancelled(config->cancel_user);
}

static int transport_valid(
    const h2_h2loader_host_managed_transport_t *transport) {
    return transport != NULL && transport->vtable != NULL &&
        transport->vtable->connect != NULL &&
        transport->vtable->stage != NULL &&
        transport->vtable->activate != NULL &&
        transport->vtable->disconnect != NULL &&
        transport->vtable->rediscover != NULL;
}

static int stage_transport_valid(
    const h2_h2loader_host_managed_transport_t *transport) {
    return transport != NULL && transport->vtable != NULL &&
        transport->vtable->connect != NULL &&
        transport->vtable->stage != NULL &&
        transport->vtable->disconnect != NULL &&
        transport->vtable->rediscover != NULL;
}

static h2_pal_result_t disconnect_after(
    const h2_h2loader_host_managed_operation_config_t *config,
    h2_pal_result_t rc) {
    h2_pal_result_t close_rc =
        config->transport.vtable->disconnect(config->transport.user);
    return rc == H2_PAL_OK ? close_rc : rc;
}

h2_pal_result_t h2_h2loader_host_managed_operation_run(
    const h2_h2loader_host_managed_operation_config_t *config,
    h2_h2loader_host_status_t *out_final_status) {
    h2_h2loader_host_status_t status;
    uint32_t attempts;
    uint32_t delay_ms;
    h2_pal_result_t rc;

    if (out_final_status != NULL) {
        memset(out_final_status, 0, sizeof(*out_final_status));
    }
    if (config == NULL || out_final_status == NULL ||
        config->time == NULL || !transport_valid(&config->transport) ||
        config->asset == NULL || config->read_payload == NULL ||
        config->asset->operation !=
            H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (operation_cancelled(config)) {
        emit_event(
            config, H2_H2LOADER_HOST_OPERATION_CONNECT, H2_PAL_EXIT);
        return H2_PAL_EXIT;
    }
    emit_event(config, H2_H2LOADER_HOST_OPERATION_CONNECT, H2_PAL_OK);
    memset(&status, 0, sizeof(status));
    rc = config->transport.vtable->connect(
        config->transport.user, &status);
    if (rc != H2_PAL_OK) {
        emit_event(config, H2_H2LOADER_HOST_OPERATION_CONNECT, rc);
        return rc;
    }
    emit_event(config, H2_H2LOADER_HOST_OPERATION_PRECHECK, H2_PAL_OK);
    if (strcmp(status.board, config->asset->board) != 0 ||
        strcmp(status.target, config->asset->target) != 0) {
        rc = disconnect_after(config, H2_PAL_ERR_INVALID_STATE);
        emit_event(config, H2_H2LOADER_HOST_OPERATION_PRECHECK, rc);
        return rc;
    }
    if (operation_cancelled(config)) {
        rc = disconnect_after(config, H2_PAL_EXIT);
        emit_event(
            config, H2_H2LOADER_HOST_OPERATION_PRECHECK, rc);
        return rc;
    }
    emit_event(config, H2_H2LOADER_HOST_OPERATION_STAGE, H2_PAL_OK);
    rc = config->transport.vtable->stage(
        config->transport.user,
        config->asset,
        config->read_payload,
        config->payload_user,
        config->is_cancelled,
        config->cancel_user,
        config->on_progress,
        config->progress_user);
    if (rc != H2_PAL_OK) {
        rc = disconnect_after(config, rc);
        emit_event(config, H2_H2LOADER_HOST_OPERATION_STAGE, rc);
        return rc;
    }
    if (operation_cancelled(config)) {
        rc = disconnect_after(config, H2_PAL_EXIT);
        emit_event(config, H2_H2LOADER_HOST_OPERATION_STAGE, rc);
        return rc;
    }
    emit_event(config, H2_H2LOADER_HOST_OPERATION_ACTIVATE, H2_PAL_OK);
    rc = config->transport.vtable->activate(
        config->transport.user, config->asset);
    rc = disconnect_after(config, rc);
    if (rc != H2_PAL_OK) {
        emit_event(config, H2_H2LOADER_HOST_OPERATION_ACTIVATE, rc);
        return rc;
    }

    attempts = config->reconnect_attempts == 0u
        ? H2_H2LOADER_HOST_DEFAULT_RECONNECT_ATTEMPTS
        : config->reconnect_attempts;
    delay_ms = config->reconnect_delay_ms == 0u
        ? H2_H2LOADER_HOST_DEFAULT_RECONNECT_DELAY_MS
        : config->reconnect_delay_ms;
    rc = H2_PAL_ERR_TIMEOUT;
    for (uint32_t attempt = 0u; attempt < attempts; ++attempt) {
        if (operation_cancelled(config)) {
            emit_event(
                config,
                H2_H2LOADER_HOST_OPERATION_REDISCOVER,
                H2_PAL_EXIT);
            return H2_PAL_EXIT;
        }
        h2_pal_result_t sleep_rc =
            h2_pal_time_sleep_ms(config->time, delay_ms);
        if (sleep_rc != H2_PAL_OK) {
            emit_event(
                config,
                H2_H2LOADER_HOST_OPERATION_REDISCOVER,
                sleep_rc);
            return sleep_rc;
        }
        emit_event(
            config, H2_H2LOADER_HOST_OPERATION_REDISCOVER, H2_PAL_OK);
        rc = config->transport.vtable->rediscover(
            config->transport.user);
        if (rc != H2_PAL_OK) {
            continue;
        }
        memset(&status, 0, sizeof(status));
        rc = config->transport.vtable->connect(
            config->transport.user, &status);
        if (rc != H2_PAL_OK) {
            continue;
        }
        rc = h2_h2loader_host_status_verify_asset(
            &status, config->asset);
        rc = disconnect_after(config, rc);
        if (rc == H2_PAL_OK) {
            *out_final_status = status;
            emit_event(
                config,
                H2_H2LOADER_HOST_OPERATION_FINAL_VERIFY,
                H2_PAL_OK);
            emit_event(
                config,
                H2_H2LOADER_HOST_OPERATION_COMPLETE,
                H2_PAL_OK);
            return H2_PAL_OK;
        }
    }
    emit_event(
        config, H2_H2LOADER_HOST_OPERATION_FINAL_VERIFY, rc);
    return rc;
}

h2_pal_result_t h2_h2loader_host_stage_operation_run(
    const h2_h2loader_host_managed_operation_config_t *config,
    h2_h2loader_host_status_t *out_final_status) {
    h2_h2loader_host_status_t status;
    uint32_t attempts;
    uint32_t delay_ms;
    h2_pal_result_t rc;

    if (out_final_status != NULL) {
        memset(out_final_status, 0, sizeof(*out_final_status));
    }
    if (config == NULL || out_final_status == NULL || config->time == NULL ||
        !stage_transport_valid(&config->transport) || config->asset == NULL ||
        config->read_payload == NULL || config->asset->operation !=
            H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (operation_cancelled(config)) {
        return H2_PAL_EXIT;
    }
    memset(&status, 0, sizeof(status));
    rc = config->transport.vtable->connect(config->transport.user, &status);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (strcmp(status.board, config->asset->board) != 0 ||
        strcmp(status.target, config->asset->target) != 0 ||
        h2_h2loader_host_status_active_role(&status) !=
            H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER) {
        return disconnect_after(config, H2_PAL_ERR_INVALID_STATE);
    }
    rc = config->transport.vtable->stage(
        config->transport.user,
        config->asset,
        config->read_payload,
        config->payload_user,
        config->is_cancelled,
        config->cancel_user,
        config->on_progress,
        config->progress_user);
    if (rc == H2_PAL_OK &&
        config->transport.vtable->read_status != NULL) {
        memset(&status, 0, sizeof(status));
        rc = config->transport.vtable->read_status(
            config->transport.user, &status);
        if (rc == H2_PAL_OK &&
            (strcmp(status.board, config->asset->board) != 0 ||
             strcmp(status.target, config->asset->target) != 0 ||
             h2_h2loader_host_status_active_role(&status) !=
                 H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER ||
             !h2_h2loader_host_status_staged_valid(&status) ||
             status.staged_bytes != config->asset->bytes ||
             strcmp(status.staged_checksum, config->asset->sha256) != 0)) {
            rc = H2_PAL_ERR_INVALID_STATE;
        }
        if (rc == H2_PAL_OK) {
            *out_final_status = status;
            return disconnect_after(config, H2_PAL_OK);
        }
        /* A large package can be fully acknowledged before the device has
           finished publishing and re-reading staged metadata. Treat a failed
           same-session status read as a reconnect boundary and verify the
           durable staged identity below instead of reporting a false send
           failure after every byte was accepted. */
        (void)disconnect_after(config, rc);
    } else {
        rc = disconnect_after(config, rc);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }

    attempts = config->reconnect_attempts == 0u
        ? H2_H2LOADER_HOST_DEFAULT_RECONNECT_ATTEMPTS
        : config->reconnect_attempts;
    delay_ms = config->reconnect_delay_ms == 0u
        ? H2_H2LOADER_HOST_DEFAULT_RECONNECT_DELAY_MS
        : config->reconnect_delay_ms;
    rc = H2_PAL_ERR_TIMEOUT;
    for (uint32_t attempt = 0u; attempt < attempts; ++attempt) {
        if (operation_cancelled(config)) {
            return H2_PAL_EXIT;
        }
        rc = h2_pal_time_sleep_ms(config->time, delay_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = config->transport.vtable->rediscover(config->transport.user);
        if (rc != H2_PAL_OK) {
            continue;
        }
        memset(&status, 0, sizeof(status));
        rc = config->transport.vtable->connect(config->transport.user, &status);
        if (rc != H2_PAL_OK) {
            continue;
        }
        if (strcmp(status.board, config->asset->board) == 0 &&
            strcmp(status.target, config->asset->target) == 0 &&
            h2_h2loader_host_status_active_role(&status) ==
                H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER &&
            h2_h2loader_host_status_staged_valid(&status) &&
            status.staged_bytes == config->asset->bytes &&
            strcmp(status.staged_checksum, config->asset->sha256) == 0) {
            *out_final_status = status;
            return disconnect_after(config, H2_PAL_OK);
        }
        rc = disconnect_after(config, H2_PAL_ERR_INVALID_STATE);
    }
    return rc;
}

h2_pal_result_t h2_h2loader_host_upgrade_tracker_init(
    const h2_h2loader_host_status_t *status,
    h2_h2loader_host_upgrade_tracker_t *out_tracker) {
    if (out_tracker != NULL) {
        memset(out_tracker, 0, sizeof(*out_tracker));
    }
    if (status == NULL || out_tracker == NULL ||
        h2_h2loader_host_status_active_role(status) !=
            H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER ||
        !h2_h2loader_host_status_staged_valid(status) ||
        !h2_h2loader_host_is_safe_identity(status->board) ||
        !h2_h2loader_host_is_safe_identity(status->target) ||
        !h2_h2loader_host_is_sha256(status->staged_checksum)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(out_tracker->board, status->board, strlen(status->board) + 1u);
    memcpy(out_tracker->target, status->target, strlen(status->target) + 1u);
    memcpy(
        out_tracker->package_sha256,
        status->staged_checksum,
        strlen(status->staged_checksum) + 1u);
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_upgrade_tracker_observe(
    h2_h2loader_host_upgrade_tracker_t *tracker,
    const h2_h2loader_host_status_t *status) {
    if (tracker == NULL || status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!h2_h2loader_host_is_safe_identity(status->board) ||
        !h2_h2loader_host_is_safe_identity(status->target) ||
        h2_h2loader_host_status_upgrade_phase(status) == 0u ||
        (status->upgrade_package_sha256[0] != '\0' &&
         !h2_h2loader_host_is_sha256(status->upgrade_package_sha256))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (strcmp(status->board, tracker->board) != 0 ||
        strcmp(status->target, tracker->target) != 0 ||
        h2_h2loader_host_status_active_role(status) !=
            H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (status->upgrade_last != 0 ||
        h2_h2loader_host_status_upgrade_phase(status) == 5u ||
        h2_h2loader_host_status_upgrade_phase(status) == 6u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (h2_h2loader_host_status_upgrade_phase(status) == 2u ||
        h2_h2loader_host_status_upgrade_phase(status) == 3u ||
        h2_h2loader_host_status_upgrade_phase(status) == 4u) {
        tracker->observed_transition = 1u;
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (h2_h2loader_host_status_upgrade_phase(status) == 1u &&
        tracker->observed_transition != 0u &&
        !h2_h2loader_host_status_staged_valid(status) &&
        strcmp(
            status->upgrade_package_sha256,
            tracker->package_sha256) == 0) {
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_WOULD_BLOCK;
}
