#include "h2_quectel_internal.h"

#include <stdio.h>
#include <string.h>

h2_pal_result_t h2_quectel_state_lock(h2_quectel_modem_t *modem) {
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return modem->lock != NULL
        ? h2_pal_mutex_lock(modem->config.sync_api, modem->lock) : H2_PAL_OK;
}

void h2_quectel_state_unlock(h2_quectel_modem_t *modem) {
    if (modem->lock != NULL) {
        (void)h2_pal_mutex_unlock(modem->config.sync_api, modem->lock);
    }
}

h2_pal_result_t h2_quectel_operation_begin(h2_quectel_modem_t *modem) {
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->operation_lock != NULL) {
        h2_pal_result_t rc = h2_pal_mutex_lock(modem->config.sync_api, modem->operation_lock);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    /* Nested operations retain just one state lock acquisition. The command
     * path can release it during transport waits while retaining operation_lock. */
    if (modem->operation_depth == 0u) {
        h2_pal_result_t rc = h2_quectel_state_lock(modem);
        if (rc != H2_PAL_OK) {
            if (modem->operation_lock != NULL) {
                (void)h2_pal_mutex_unlock(modem->config.sync_api, modem->operation_lock);
            }
            return rc;
        }
    }
    modem->operation_depth++;
    return H2_PAL_OK;
}

h2_pal_result_t h2_quectel_power_wake(h2_quectel_modem_t *modem) {
    if ((modem->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) == 0u) {
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = modem->config.sleep_gate(modem->config.transport_user, 0);
    modem->sleep_allowed = 0u;
    if (rc != H2_PAL_OK) {
        modem->power_fault = 1u;
    }
    return rc;
}

h2_pal_result_t h2_quectel_power_reconcile(h2_quectel_modem_t *modem, h2_pal_result_t result) {
    if (modem->opened != 0u && modem->power_configured != 0u && modem->power_fault == 0u &&
        modem->power_policy == H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP && modem->gnss_hold == 0u &&
        modem->call_hold == 0u && modem->data_hold == 0u && modem->sleep_allowed == 0u) {
        h2_pal_result_t rc = modem->config.sleep_gate(modem->config.transport_user, 1);
        if (rc == H2_PAL_OK) {
            modem->sleep_allowed = 1u;
        } else {
            modem->power_fault = 1u;
            /* Restore the safer level after a partially applied sleep gate. */
            (void)h2_quectel_power_wake(modem);
            if (result == H2_PAL_OK) {
                result = rc;
            }
        }
    }
    return result;
}

h2_pal_result_t h2_quectel_operation_end(h2_quectel_modem_t *modem, h2_pal_result_t result) {
    modem->operation_depth--;
    if (modem->operation_depth == 0u) {
        result = h2_quectel_power_reconcile(modem, result);
    }
    if (modem->operation_depth == 0u) {
        h2_quectel_state_unlock(modem);
    }
    if (modem->operation_lock != NULL) {
        h2_pal_result_t rc = h2_pal_mutex_unlock(modem->config.sync_api, modem->operation_lock);
        if (result == H2_PAL_OK) {
            result = rc;
        }
    }
    return result;
}

h2_pal_result_t h2_quectel_power_prepare(h2_quectel_modem_t *modem) {
    int low_power = (modem->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u;
    if (!low_power && modem->config.sim_hotplug == 0u) {
        return H2_PAL_OK;
    }
    if (modem->model_checked == 0u) {
        h2_quectel_response_t response;
        h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT+CGMM", &response, 0);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (response.count == 0u || (strcmp(response.lines[0], "EC25") != 0 &&
                                     strncmp(response.lines[0], "EC25-", 5u) != 0)) {
            modem->capabilities &= ~(uint32_t)H2_PAL_MODEM_CAPABILITY_LOW_POWER;
            return H2_PAL_ERR_UNSUPPORTED;
        }
        modem->model_checked = 1u;
    }
    if (modem->sim_restart_required != 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (modem->config.sim_hotplug != 0u) {
        h2_quectel_response_t response;
        h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT+QSIMDET?", &response, 0);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        int enabled = -1;
        int level = -1;
        const char *line = h2_quectel_response_find(&response, "+QSIMDET:");
        if (line == NULL || sscanf(line, "+QSIMDET: %d,%d", &enabled, &level) != 2) {
            return H2_PAL_ERR_FORMAT;
        }
        if (enabled != 1 || level != modem->config.sim_insert_level) {
            char cmd[32];
            (void)snprintf(cmd, sizeof(cmd), "AT+QSIMDET=1,%u", modem->config.sim_insert_level);
            rc = h2_quectel_at_exchange(modem, cmd, NULL, 0);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            modem->sim_restart_required = 1u;
            return H2_PAL_ERR_INVALID_STATE;
        }
        rc = h2_quectel_at_exchange(modem, "AT+QSIMSTAT=1", NULL, 0);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    if (low_power) {
        h2_pal_result_t rc = h2_quectel_at_exchange(
            modem,
            modem->power_policy == H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP ? "AT+QSCLK=1"
                                                                        : "AT+QSCLK=0",
            NULL, 0);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        modem->power_configured = 1u;
        modem->power_fault = 0u;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_quectel_set_power_policy(void *user, h2_pal_modem_power_policy_t policy) {
    h2_quectel_modem_t *modem = user;
    if (policy != H2_PAL_MODEM_POWER_POLICY_ACTIVE &&
        policy != H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_quectel_operation_begin(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((modem->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) == 0u) {
        return h2_quectel_operation_end(modem, H2_PAL_ERR_UNSUPPORTED);
    }
    if (modem->opened == 0u) {
        return h2_quectel_operation_end(modem, H2_PAL_ERR_INVALID_STATE);
    }
    rc = h2_quectel_modem_prepare(modem);
    if (rc != H2_PAL_OK) {
        return h2_quectel_operation_end(modem, rc);
    }
    h2_pal_modem_power_policy_t saved = modem->power_policy;
    rc = h2_quectel_at_exchange(
        modem, policy == H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP ? "AT+QSCLK=1" : "AT+QSCLK=0", NULL,
        0);
    if (rc == H2_PAL_OK) {
        modem->power_policy = policy;
        modem->power_fault = 0u;
        modem->power_configured = 1u;
    }
    rc = h2_quectel_power_reconcile(modem, rc);
    if (rc != H2_PAL_OK) {
        modem->power_policy = saved;
    }
    return h2_quectel_operation_end(modem, rc);
}

h2_pal_result_t h2_quectel_get_power_status(void *user, h2_pal_modem_power_status_t *out_status) {
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_status->policy = H2_PAL_MODEM_POWER_POLICY_ACTIVE;
    out_status->state = H2_PAL_MODEM_POWER_STATE_UNKNOWN;
    h2_quectel_modem_t *modem = user;
    h2_pal_result_t rc = h2_quectel_state_lock(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((modem->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) == 0u) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else if (modem->opened == 0u) {
        rc = H2_PAL_ERR_INVALID_STATE;
    } else {
        out_status->policy = modem->power_policy;
        /* DTR/configuration are intentions, not a physical sleep sensor. */
    }
    h2_quectel_state_unlock(modem);
    return rc;
}
