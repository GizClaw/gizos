#include "h2_smoke_ble_wifi_config.h"

#include "h2_ble_wifi_config.h"

#include <stdio.h>
#include <string.h>

#define H2_SMOKE_BLE_WIFI_CONFIG_ADV_INTERVAL_MIN_MS 100u
#define H2_SMOKE_BLE_WIFI_CONFIG_ADV_INTERVAL_MAX_MS 150u
#define H2_SMOKE_BLE_WIFI_CONFIG_LOCAL_NAME "H2-Provision"

typedef struct h2_smoke_ble_wifi_config_context {
    h2_runtime_t *runtime;
    /*
     * The service worker task publishes the outcome and the app task waits
     * for it, so both go through the mutex and the condition variable.
     */
    h2_pal_mutex_t *mutex;
    h2_pal_cond_t *cond;
    int provisioned;
} h2_smoke_ble_wifi_config_context_t;

static const char *h2_smoke_ble_wifi_config_event_name(
    h2_ble_wifi_config_event_t event) {
    switch (event) {
    case H2_BLE_WIFI_CONFIG_EVENT_CONNECTED:
        return "connected";
    case H2_BLE_WIFI_CONFIG_EVENT_DISCONNECTED:
        return "disconnected";
    case H2_BLE_WIFI_CONFIG_EVENT_MTU_TOO_SMALL:
        return "mtu_too_small";
    case H2_BLE_WIFI_CONFIG_EVENT_SCAN_STARTED:
        return "scan_started";
    case H2_BLE_WIFI_CONFIG_EVENT_SCAN_FINISHED:
        return "scan_finished";
    case H2_BLE_WIFI_CONFIG_EVENT_CREDENTIALS_RECEIVED:
        return "credentials_received";
    case H2_BLE_WIFI_CONFIG_EVENT_PROVISION_SUCCEEDED:
        return "provision_succeeded";
    case H2_BLE_WIFI_CONFIG_EVENT_PROVISION_FAILED:
        return "provision_failed";
    case H2_BLE_WIFI_CONFIG_EVENT_PROTOCOL_ERROR:
        return "protocol_error";
    default:
        return "unknown";
    }
}

/*
 * Runs on the service worker task. It must return promptly, so it only
 * prints and publishes a flag the app task polls.
 */
static void h2_smoke_ble_wifi_config_on_event(
    void *user,
    h2_ble_wifi_config_t *service,
    h2_ble_wifi_config_event_t event,
    uint16_t conn_handle,
    int status) {
    h2_smoke_ble_wifi_config_context_t *context = user;
    (void)service;
    printf(
        "H2_SMOKE_BLE_WIFI_CONFIG event=%s conn=%u status=%d\n",
        h2_smoke_ble_wifi_config_event_name(event), (unsigned)conn_handle,
        status);
    fflush(stdout);
    if (event != H2_BLE_WIFI_CONFIG_EVENT_PROVISION_SUCCEEDED) {
        return;
    }
    (void)h2_pal_mutex_lock(context->runtime->sync, context->mutex);
    context->provisioned = 1;
    (void)h2_pal_cond_broadcast(context->runtime->sync, context->cond);
    (void)h2_pal_mutex_unlock(context->runtime->sync, context->mutex);
}

static void h2_smoke_ble_wifi_config_print_uuid(
    const char *label,
    const uint8_t uuid[16]) {
    /* Stored in ATT byte order; print the human form. */
    printf("H2_SMOKE_BLE_WIFI_CONFIG %s=", label);
    for (size_t i = 0u; i < 16u; ++i) {
        size_t index = 15u - i;
        printf("%02x", uuid[index]);
        if (i == 3u || i == 5u || i == 7u || i == 9u) {
            printf("-");
        }
    }
    printf("\n");
}

static void h2_smoke_ble_wifi_config_print_station(h2_runtime_t *runtime) {
    h2_pal_wifi_sta_status_t status;
    memset(&status, 0, sizeof(status));
    if (h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status) != H2_PAL_OK) {
        printf("H2_SMOKE_BLE_WIFI_CONFIG station=unavailable\n");
        fflush(stdout);
        return;
    }
    uint8_t ip[4] = { 0u, 0u, 0u, 0u };
    h2_pal_wifi_ip4_to_bytes(status.ip.ip4, ip);
    printf(
        "H2_SMOKE_BLE_WIFI_CONFIG station state=%d ssid=%.*s rssi=%d "
        "ip=%u.%u.%u.%u ip_valid=%u\n",
        (int)status.state, (int)status.ssid_len, status.ssid, status.rssi,
        (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3],
        (unsigned)status.ip_valid);
    fflush(stdout);
}

int h2_smoke_ble_wifi_config_run(h2_runtime_t *runtime) {
    if (runtime == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (runtime->ble_host == NULL || runtime->wifi_sta == NULL ||
        runtime->task == NULL || runtime->sync == NULL ||
        runtime->system_event == NULL || runtime->mem == NULL ||
        runtime->time == NULL) {
        printf("H2_SMOKE_BLE_WIFI_CONFIG stage=capabilities rc=%d\n",
               H2_PAL_ERR_UNSUPPORTED);
        fflush(stdout);
        return H2_PAL_ERR_UNSUPPORTED;
    }

    h2_smoke_ble_wifi_config_context_t context;
    memset(&context, 0, sizeof(context));
    context.runtime = runtime;

    const h2_pal_mutex_config_t mutex_config = {
        .name = "ble-wifi-config-app",
        .allocator = runtime->mem,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    int rc = h2_pal_mutex_create(runtime->sync, &mutex_config, &context.mutex);
    if (rc == H2_PAL_OK) {
        const h2_pal_cond_config_t cond_config = {
            .name = "ble-wifi-config-app",
            .allocator = runtime->mem,
        };
        rc = h2_pal_cond_create(runtime->sync, &cond_config, &context.cond);
        if (rc != H2_PAL_OK) {
            (void)h2_pal_mutex_destroy(runtime->sync, context.mutex);
        }
    }
    if (rc != H2_PAL_OK) {
        printf("H2_SMOKE_BLE_WIFI_CONFIG stage=sync rc=%d\n", rc);
        fflush(stdout);
        return rc;
    }

    const h2_ble_wifi_config_api_t api = {
        .runtime = runtime,
        .ble = runtime->ble_host,
        .wifi_sta = runtime->wifi_sta,
        .task = runtime->task,
        .sync = runtime->sync,
        .system_event = runtime->system_event,
        .allocator = runtime->mem,
    };
    h2_ble_wifi_config_config_t config;
    memset(&config, 0, sizeof(config));
    config.on_event = h2_smoke_ble_wifi_config_on_event;
    config.user = &context;

    h2_ble_wifi_config_t *service = NULL;
    rc = h2_ble_wifi_config_open(&api, &config, &service);
    printf("H2_SMOKE_BLE_WIFI_CONFIG stage=open rc=%d\n", rc);
    fflush(stdout);
    if (rc != H2_PAL_OK) {
        (void)h2_pal_cond_destroy(runtime->sync, context.cond);
        (void)h2_pal_mutex_destroy(runtime->sync, context.mutex);
        return rc;
    }

    /*
     * Legacy connectable advertising carrying the service UUID: that is what
     * the phone applications filter on. The library pauses this advertising
     * for the duration of a scan or a connect attempt.
     */
    const h2_pal_ble_uuid_t service_uuid = {
        .data = h2_ble_wifi_config_default_service_uuid,
        .len = 16u,
    };
    const h2_pal_ble_adv_data_t data = {
        .local_name = H2_SMOKE_BLE_WIFI_CONFIG_LOCAL_NAME,
        .service_uuids = &service_uuid,
        .service_uuid_count = 1u,
    };
    const h2_pal_ble_adv_params_t params = {
        .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
        .interval_min_ms = H2_SMOKE_BLE_WIFI_CONFIG_ADV_INTERVAL_MIN_MS,
        .interval_max_ms = H2_SMOKE_BLE_WIFI_CONFIG_ADV_INTERVAL_MAX_MS,
        .type = H2_PAL_BLE_ADV_TYPE_LEGACY,
    };
    rc = h2_ble_wifi_config_start_advertising(service, &data, &params);
    printf("H2_SMOKE_BLE_WIFI_CONFIG stage=advertising rc=%d name=%s\n", rc,
           H2_SMOKE_BLE_WIFI_CONFIG_LOCAL_NAME);
    fflush(stdout);
    if (rc == H2_PAL_OK) {
        h2_smoke_ble_wifi_config_print_uuid(
            "service", h2_ble_wifi_config_default_service_uuid);
        h2_smoke_ble_wifi_config_print_uuid(
            "command", h2_ble_wifi_config_default_command_uuid);
        h2_smoke_ble_wifi_config_print_uuid(
            "scan", h2_ble_wifi_config_default_scan_uuid);
        h2_smoke_ble_wifi_config_print_uuid(
            "provision", h2_ble_wifi_config_default_provision_uuid);
        printf("H2_SMOKE_BLE_WIFI_CONFIG stage=waiting window_ms=%u\n",
               (unsigned)H2_SMOKE_BLE_WIFI_CONFIG_WINDOW_MS);
        fflush(stdout);
    }

    if (rc == H2_PAL_OK) {
        /*
         * Wait against a deadline, not a fresh timeout per iteration: a
         * condition wait may wake without the outcome being set, and
         * re-arming the full window each time would leave the GATT service
         * writable past the advertised maximum.
         */
        uint64_t started_ms = 0u;
        rc = h2_pal_time_get_monotonic_ms(runtime->time, &started_ms);
        (void)h2_pal_mutex_lock(runtime->sync, context.mutex);
        while (rc == H2_PAL_OK && context.provisioned == 0) {
            uint64_t now_ms = 0u;
            rc = h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
            if (rc != H2_PAL_OK) {
                break;
            }
            uint64_t elapsed_ms = now_ms - started_ms;
            if (elapsed_ms >= (uint64_t)H2_SMOKE_BLE_WIFI_CONFIG_WINDOW_MS) {
                rc = H2_PAL_ERR_TIMEOUT;
                break;
            }
            rc = h2_pal_cond_wait(
                runtime->sync, context.cond, context.mutex,
                (uint32_t)((uint64_t)H2_SMOKE_BLE_WIFI_CONFIG_WINDOW_MS -
                           elapsed_ms));
        }
        (void)h2_pal_mutex_unlock(runtime->sync, context.mutex);
    }

    h2_ble_wifi_config_stats_t stats;
    if (h2_ble_wifi_config_get_stats(service, &stats) == H2_PAL_OK) {
        printf(
            "H2_SMOKE_BLE_WIFI_CONFIG stats mtu=%u scans=%u aps=%u dropped=%u "
            "notify_failures=%u attempts=%u failures=%u protocol_errors=%u "
            "peer_changes=%u\n",
            (unsigned)stats.att_mtu, (unsigned)stats.scans_started,
            (unsigned)stats.aps_reported, (unsigned)stats.aps_dropped,
            (unsigned)stats.notify_failures, (unsigned)stats.provision_attempts,
            (unsigned)stats.provision_failures, (unsigned)stats.protocol_errors,
            (unsigned)stats.sends_during_peer_change);
        fflush(stdout);
    }
    (void)h2_pal_mutex_lock(runtime->sync, context.mutex);
    int provisioned = context.provisioned;
    (void)h2_pal_mutex_unlock(runtime->sync, context.mutex);
    if (provisioned != 0) {
        h2_smoke_ble_wifi_config_print_station(runtime);
    }

    /*
     * The provisioning window is the authorization, so close it as soon as
     * the device is provisioned instead of leaving credentials writable.
     */
    int close_rc = h2_ble_wifi_config_close(service);
    printf("H2_SMOKE_BLE_WIFI_CONFIG stage=close rc=%d\n", close_rc);
    fflush(stdout);
    if (rc == H2_PAL_OK && close_rc != H2_PAL_OK) {
        rc = close_rc;
    }
    (void)h2_pal_cond_destroy(runtime->sync, context.cond);
    (void)h2_pal_mutex_destroy(runtime->sync, context.mutex);
    printf("H2_SMOKE_BLE_WIFI_CONFIG stage=done rc=%d\n", rc);
    fflush(stdout);
    return rc;
}
