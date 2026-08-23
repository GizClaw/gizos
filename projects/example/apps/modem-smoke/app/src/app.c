#include "h2_modem_smoke.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define H2_MODEM_SMOKE_OPEN_TIMEOUT_MS 10000u
#define H2_MODEM_SMOKE_CLOSE_TIMEOUT_MS 10000u
#define H2_MODEM_SMOKE_POLL_INTERVAL_MS 1000u

static void log_message(
    h2_runtime_t *runtime,
    h2_pal_log_level_t level,
    const char *message) {
    (void)h2_pal_log_write(runtime->log, level, "modem-smoke", message);
}

static void log_stage_rc(
    h2_runtime_t *runtime,
    h2_pal_log_level_t level,
    const char *stage,
    h2_pal_result_t rc) {
    char message[H2_PAL_LOG_MESSAGE_MAX];
    (void)snprintf(message, sizeof(message), "stage=%s rc=%d", stage, rc);
    log_message(runtime, level, message);
}

static h2_pal_result_t wait_for_registration(
    h2_runtime_t *runtime,
    uint32_t timeout_ms,
    h2_pal_modem_status_t *out_status) {
    uint64_t start_ms = 0u;
    h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(runtime->time, &start_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const uint64_t deadline_ms = h2_pal_time_deadline_ms(start_ms, timeout_ms);
    do {
        rc = h2_pal_modem_get_status(runtime->modem, out_status);
        if (rc != H2_PAL_OK ||
            out_status->registration == H2_PAL_MODEM_REGISTRATION_HOME ||
            out_status->registration == H2_PAL_MODEM_REGISTRATION_ROAMING) {
            return rc;
        }
        rc = h2_pal_time_sleep_ms(runtime->time, H2_MODEM_SMOKE_POLL_INTERVAL_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        uint64_t now_ms = 0u;
        rc = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (h2_pal_time_deadline_expired(now_ms, deadline_ms)) {
            return H2_PAL_ERR_TIMEOUT;
        }
    } while (true);
}

static void log_identity(h2_runtime_t *runtime, const h2_pal_modem_identity_t *identity) {
    char message[H2_PAL_LOG_MESSAGE_MAX];
    (void)snprintf(
        message,
        sizeof(message),
        "stage=identity manufacturer=%.32s model=%.32s revision=%.32s imei=%.16s",
        identity->manufacturer,
        identity->model,
        identity->revision,
        identity->imei);
    log_message(runtime, H2_PAL_LOG_INFO, message);
}

static void log_modem_status(
    h2_runtime_t *runtime,
    const char *stage,
    const h2_pal_modem_status_t *status) {
    const char *sim = "unknown";
    switch (status->sim) {
        case H2_PAL_MODEM_SIM_STATE_ABSENT:
            sim = "absent";
            break;
        case H2_PAL_MODEM_SIM_STATE_LOCKED:
            sim = "locked";
            break;
        case H2_PAL_MODEM_SIM_STATE_READY:
            sim = "ready";
            break;
        default:
            break;
    }
    char message[H2_PAL_LOG_MESSAGE_MAX];
    (void)snprintf(
        message,
        sizeof(message),
        "stage=%s sim=%s sim_code=%d registration=%d packet=%d rat=%d",
        stage,
        sim,
        status->sim,
        status->registration,
        status->packet,
        status->rat);
    log_message(
        runtime,
        status->sim == H2_PAL_MODEM_SIM_STATE_READY
            ? H2_PAL_LOG_INFO
            : H2_PAL_LOG_WARN,
        message);
}

static h2_pal_result_t log_data_status(h2_runtime_t *runtime) {
    h2_pal_modem_data_status_t status;
    h2_pal_result_t rc = h2_pal_modem_get_data_status(runtime->modem, &status);
    if (rc == H2_PAL_OK &&
        (status.state != H2_PAL_MODEM_DATA_OPEN || status.ip4_valid == 0u)) {
        rc = H2_PAL_ERR_INVALID_STATE;
    }
    if (rc != H2_PAL_OK) {
        log_stage_rc(runtime, H2_PAL_LOG_ERROR, "ppp_status", rc);
        return rc;
    }
    const uint8_t *ip = (const uint8_t *)&status.ip4;
    char message[H2_PAL_LOG_MESSAGE_MAX];
    (void)snprintf(
        message,
        sizeof(message),
        "stage=ppp_status state=%d ip=%u.%u.%u.%u",
        status.state,
        (unsigned)ip[0],
        (unsigned)ip[1],
        (unsigned)ip[2],
        (unsigned)ip[3]);
    log_message(runtime, H2_PAL_LOG_INFO, message);
    return H2_PAL_OK;
}

static void close_modem(h2_runtime_t *runtime) {
    const h2_pal_result_t rc = h2_pal_modem_close(
        runtime->modem, H2_MODEM_SMOKE_CLOSE_TIMEOUT_MS);
    log_stage_rc(
        runtime,
        rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
        "close",
        rc);
}

h2_pal_result_t h2_modem_smoke_run(
    h2_runtime_t *runtime,
    const h2_modem_smoke_config_t *config) {
    if (runtime == NULL || runtime->modem == NULL || runtime->net == NULL ||
        runtime->time == NULL || runtime->log == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const uint32_t registration_timeout_ms =
        config != NULL && config->registration_timeout_ms != 0u
            ? config->registration_timeout_ms
            : 60000u;
    const uint32_t data_timeout_ms =
        config != NULL && config->data_timeout_ms != 0u
            ? config->data_timeout_ms
            : 90000u;
    const uint32_t ping_timeout_ms =
        config != NULL && config->ping_timeout_ms != 0u
            ? config->ping_timeout_ms
            : 10000u;

    h2_pal_result_t rc = h2_pal_modem_open(runtime->modem, H2_MODEM_SMOKE_OPEN_TIMEOUT_MS);
    log_stage_rc(
        runtime,
        rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
        "open",
        rc);
    if (rc != H2_PAL_OK) {
        log_message(runtime, H2_PAL_LOG_WARN, "stage=sim attempted=0 reason=modem_unavailable");
        log_message(
            runtime,
            H2_PAL_LOG_WARN,
            "stage=registration attempted=0 reason=modem_unavailable");
        log_message(runtime, H2_PAL_LOG_WARN, "stage=ppp attempted=0 reason=modem_unavailable");
        log_message(runtime, H2_PAL_LOG_WARN, "stage=icmp attempted=0 reason=modem_unavailable");
        close_modem(runtime);
        return H2_PAL_OK;
    }

    h2_pal_modem_identity_t identity;
    rc = h2_pal_modem_get_identity(runtime->modem, &identity);
    if (rc == H2_PAL_OK) {
        log_identity(runtime, &identity);
    } else {
        log_stage_rc(runtime, H2_PAL_LOG_ERROR, "identity", rc);
    }

    h2_pal_modem_status_t status;
    rc = h2_pal_modem_get_status(runtime->modem, &status);
    if (rc == H2_PAL_OK) {
        log_modem_status(runtime, "sim", &status);
    } else {
        log_stage_rc(runtime, H2_PAL_LOG_ERROR, "sim", rc);
    }
    if (rc != H2_PAL_OK || status.sim != H2_PAL_MODEM_SIM_STATE_READY) {
        log_message(
            runtime,
            H2_PAL_LOG_WARN,
            "stage=registration attempted=0 reason=sim_unavailable");
        log_message(runtime, H2_PAL_LOG_WARN, "stage=ppp attempted=0 reason=sim_unavailable");
        log_message(runtime, H2_PAL_LOG_WARN, "stage=icmp attempted=0 reason=sim_unavailable");
        close_modem(runtime);
        return H2_PAL_OK;
    }

    rc = wait_for_registration(runtime, registration_timeout_ms, &status);
    if (rc == H2_PAL_OK) {
        log_modem_status(runtime, "registration", &status);
    } else {
        log_stage_rc(runtime, H2_PAL_LOG_ERROR, "registration", rc);
        log_message(runtime, H2_PAL_LOG_WARN, "stage=ppp attempted=0 reason=not_registered");
        log_message(runtime, H2_PAL_LOG_WARN, "stage=icmp attempted=0 reason=not_registered");
        close_modem(runtime);
        return H2_PAL_OK;
    }

    if (config != NULL && config->apn != NULL && config->apn[0] != '\0') {
        h2_pal_modem_apn_config_t apn_config;
        memset(&apn_config, 0, sizeof(apn_config));
        const int apn_len = snprintf(
            apn_config.apn, sizeof(apn_config.apn), "%s", config->apn);
        rc = apn_len < 0 || (size_t)apn_len >= sizeof(apn_config.apn)
            ? H2_PAL_ERR_INVALID_ARG
            : h2_pal_modem_set_apn(runtime->modem, &apn_config);
        log_stage_rc(
            runtime,
            rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
            "apn",
            rc);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_modem_data_open(runtime->modem, data_timeout_ms);
    }
    log_stage_rc(
        runtime,
        rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
        "ppp",
        rc);
    if (rc == H2_PAL_OK) {
        rc = log_data_status(runtime);
    }

    if (rc == H2_PAL_OK) {
        const h2_pal_net_addr_t target = {
            .family = H2_PAL_NET_FAMILY_IPV4,
            .ip = { 1u, 1u, 1u, 1u },
        };
        h2_pal_net_icmp_echo_result_t ping = { 0 };
        rc = h2_pal_net_icmp_echo(runtime->net, &target, NULL, ping_timeout_ms, &ping);
        char message[H2_PAL_LOG_MESSAGE_MAX];
        (void)snprintf(
            message,
            sizeof(message),
            "stage=icmp target=1.1.1.1 rc=%d transmitted=%" PRIu32
            " received=%" PRIu32 " elapsed_ms=%" PRIu32,
            rc,
            ping.transmitted,
            ping.received,
            ping.elapsed_ms);
        log_message(runtime, rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR, message);
    } else {
        log_message(runtime, H2_PAL_LOG_WARN, "stage=icmp attempted=0 reason=ppp_unavailable");
    }

    h2_pal_result_t close_rc = h2_pal_modem_data_close(
        runtime->modem, H2_MODEM_SMOKE_CLOSE_TIMEOUT_MS);
    log_stage_rc(
        runtime,
        close_rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
        "ppp_close",
        close_rc);
    close_modem(runtime);
    return H2_PAL_OK;
}
