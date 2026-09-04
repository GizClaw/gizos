#include "h2_loader_ble.h"
#include "h2_loader_task_names.h"

#include "h2_loader_app_client.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define H2_LOADER_BLE_ADV_INTERVAL_MIN_MS 100u
#define H2_LOADER_BLE_ADV_INTERVAL_MAX_MS 120u
#define H2_LOADER_BLE_ADV_SID 0u
#define H2_LOADER_BLE_IO_TIMEOUT_MS 5000u
#define H2_LOADER_BLE_WINDOW 32u
#define H2_LOADER_BLE_INPUT_FRAME_CAPACITY 16u
#define H2_LOADER_BLE_BUFFER_SIZE (8u * 1024u)
#define H2_LOADER_BLE_SERVICE_DATA_FIXED_LEN 11u
#define H2_LOADER_BLE_COMPACT_SERVICE_DATA_LEN 18u
#define H2_LOADER_BLE_LINK_INTERVAL_MS 15u
#define H2_LOADER_BLE_LINK_SUPERVISION_TIMEOUT_MS 4000u
#define H2_LOADER_BLE_LINK_TIMEOUT_MS 5000u
#define H2_LOADER_BLE_PEER_MTU_WAIT_MS 250u
#define H2_LOADER_BLE_ADV_RESTART_GRACE_MS 100u
#define H2_LOADER_BLE_ADV_RESUME_RETRY_COUNT 20u
#define H2_LOADER_BLE_ADV_RESUME_RETRY_DELAY_MS 50u
#define H2_LOADER_BLE_LINK_TASK_STACK_SIZE (6u * 1024u)
#define H2_LOADER_BLE_EVENT_SUBSCRIPTION_COUNT 4u
#define H2_LOADER_BLE_MAX_ADDITIONAL_ADVERTISED_SERVICES 3u

/* Advertising pause requests arrive from callbacks that must not block, so the
 * wanted state is published without taking the link mutex. */
#define H2_LOADER_BLE_ADV_PAUSE_REQUEST_NONE 0
#define H2_LOADER_BLE_ADV_PAUSE_REQUEST_PAUSE 1
#define H2_LOADER_BLE_ADV_PAUSE_REQUEST_RESUME 2

static int h2_loader_ble_open_error(const char *stage, int rc) {
    printf("H2_LOADER_BLE_ERROR stage=%s code=%d\n", stage, rc);
    return rc;
}

static void h2_loader_ble_stream_event(
    void *user,
    h2_bleikcp_t *stream,
    h2_bleikcp_event_t event,
    uint16_t conn_handle,
    int status) {
    (void)user;
    if (stream == NULL ||
        (event != H2_BLEIKCP_EVENT_DISCONNECTED &&
         event != H2_BLEIKCP_EVENT_FATAL_ERROR)) {
        return;
    }
    h2_bleikcp_stats_t stats;
    if (h2_bleikcp_get_stats(stream, &stats) != H2_PAL_OK) {
        return;
    }
    printf(
        "H2_LOADER_BLE_SESSION conn=%u event=%d status=%d "
        "tx_frames=%llu rx_frames=%llu input_errors=%llu dropped_input=%llu "
        "output_blocked=%llu output_retries=%llu waitsnd=%u "
        "input_high_water=%zu tx_high_water=%zu rx_high_water=%zu\n",
        (unsigned)conn_handle,
        (int)event,
        status,
        (unsigned long long)stats.tx_frames,
        (unsigned long long)stats.rx_frames,
        (unsigned long long)stats.input_errors,
        (unsigned long long)stats.dropped_input,
        (unsigned long long)stats.output_blocked,
        (unsigned long long)stats.output_retries,
        (unsigned)stats.waitsnd,
        stats.input_high_water,
        stats.tx_high_water,
        stats.rx_high_water);
}

const uint8_t h2_loader_ble_service_uuid_bytes[16] = {
    0x1du, 0x72u, 0xa1u, 0x6bu, 0x3au, 0xafu, 0x0bu, 0xaau,
    0xe2u, 0x53u, 0xd8u, 0x3eu, 0x70u, 0xb5u, 0xa4u, 0x71u,
};
const uint8_t h2_loader_ble_tx_uuid_bytes[16] = {
    0x1eu, 0xcfu, 0xd2u, 0xbcu, 0x8fu, 0xd3u, 0xb0u, 0x98u,
    0x70u, 0x51u, 0xfbu, 0x56u, 0x55u, 0xa0u, 0xd3u, 0x46u,
};
const uint8_t h2_loader_ble_rx_uuid_bytes[16] = {
    0xfeu, 0x0eu, 0xbcu, 0xc9u, 0xd6u, 0x87u, 0x36u, 0xa5u,
    0x8du, 0x5bu, 0xf2u, 0x05u, 0x15u, 0xadu, 0x62u, 0x8fu,
};

struct h2_loader_ble_service {
    h2_loader_ble_service_config_t config;
    h2_bleikcp_server_t *server;
    h2_pal_ble_adv_set_t *adv_set;
    h2_pal_ble_adv_params_t adv_params;
    h2_pal_system_event_subscription_t *event_subscriptions[
        H2_LOADER_BLE_EVENT_SUBSCRIPTION_COUNT];
    h2_pal_mutex_t *link_mutex;
    h2_pal_semaphore_t *link_semaphore;
    h2_pal_semaphore_t *mtu_semaphore;
    h2_pal_task_t *link_task;
    uint16_t active_conn_handle;
    uint16_t active_att_mtu;
    uint16_t pending_conn_handle;
    bool active_mtu_confirmed;
    bool link_pending;
    bool advertising_restart_pending;
    bool advertising_paused;
    /* Advertising stays stopped while any reason holds it: an explicit caller
     * pause, or the automatic Wi-Fi coexistence pause. */
    bool advertising_pause_manual;
    bool advertising_pause_auto;
    atomic_int advertising_pause_request;
    atomic_bool closing_requested;
    bool closing;
    uint8_t service_data[H2_LOADER_BLE_SERVICE_DATA_FIXED_LEN +
                         H2_LOADER_BLE_BOARD_MAX];
    size_t service_data_len;
    h2_pal_ble_uuid_t additional_services[
        H2_LOADER_BLE_MAX_ADDITIONAL_ADVERTISED_SERVICES];
    uint8_t additional_service_data[
        H2_LOADER_BLE_MAX_ADDITIONAL_ADVERTISED_SERVICES][16];
    size_t additional_service_count;
};

static bool h2_loader_ble_service_is_connected(
    h2_loader_ble_service_t *service) {
    bool connected;
    (void)h2_pal_mutex_lock(
        service->config.api.sync, service->link_mutex);
    connected = service->active_conn_handle !=
        H2_PAL_BLE_INVALID_CONN_HANDLE;
    (void)h2_pal_mutex_unlock(
        service->config.api.sync, service->link_mutex);
    return connected;
}

static bool h2_loader_ble_service_can_restart_advertising(
    h2_loader_ble_service_t *service) {
    bool can_restart;
    (void)h2_pal_mutex_lock(
        service->config.api.sync, service->link_mutex);
    can_restart = !service->closing && !service->advertising_paused &&
        service->active_conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE;
    (void)h2_pal_mutex_unlock(
        service->config.api.sync, service->link_mutex);
    return can_restart;
}

static int h2_loader_ble_service_update_advertising(
    h2_loader_ble_service_t *service,
    const h2_pal_ble_uuid_t *additional_services,
    size_t additional_service_count,
    bool stop_first) {
    h2_pal_ble_uuid_t services[
        1u + H2_LOADER_BLE_MAX_ADDITIONAL_ADVERTISED_SERVICES];
    if (service == NULL ||
        (service->config.advertising_mode ==
             H2_LOADER_BLE_ADVERTISING_EXTENDED &&
         service->adv_set == NULL) ||
        additional_service_count >
            H2_LOADER_BLE_MAX_ADDITIONAL_ADVERTISED_SERVICES ||
        (additional_service_count > 0u && additional_services == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    services[0] = (h2_pal_ble_uuid_t){
        .data = h2_loader_ble_service_uuid_bytes,
        .len = sizeof(h2_loader_ble_service_uuid_bytes),
    };
    for (size_t i = 0u; i < additional_service_count; ++i) {
        services[i + 1u] = additional_services[i];
    }
    h2_pal_ble_adv_data_t adv_data = {
        .local_name = NULL,
        .service_uuids = services,
        .service_uuid_count = additional_service_count + 1u,
    };
    if (service->config.advertising_mode ==
        H2_LOADER_BLE_ADVERTISING_LEGACY) {
        adv_data.manufacturer_data = (h2_pal_ble_bytes_t){
            .data = service->service_data,
            .len = service->service_data_len,
        };
    } else {
        adv_data.service_data_uuid = services[0];
        adv_data.service_data = (h2_pal_ble_bytes_t){
            .data = service->service_data,
            .len = service->service_data_len,
        };
    }
    const bool legacy = service->config.advertising_mode ==
        H2_LOADER_BLE_ADVERTISING_LEGACY;
    int rc = stop_first
        ? legacy
            ? h2_pal_ble_stop_advertising(service->config.api.ble)
            : h2_pal_ble_adv_set_stop(
                  service->config.api.ble, service->adv_set)
        : H2_PAL_OK;
    if (stop_first && rc == H2_PAL_ERR_INVALID_STATE) {
        rc = H2_PAL_OK;
    }
    if (rc == H2_PAL_OK) {
        rc = legacy
            ? h2_pal_ble_set_adv_data(service->config.api.ble, &adv_data)
            : h2_pal_ble_adv_set_set_data(
                  service->config.api.ble, service->adv_set, &adv_data);
    }
    if (rc == H2_PAL_OK) {
        rc = legacy
            ? h2_pal_ble_start_advertising(
                  service->config.api.ble, &service->adv_params)
            : h2_pal_ble_adv_set_start(
                  service->config.api.ble, service->adv_set);
    }
    return rc;
}

static int h2_loader_ble_service_pause_advertising_reason(
    h2_loader_ble_service_t *service,
    bool manual);
static int h2_loader_ble_service_resume_advertising_reason(
    h2_loader_ble_service_t *service,
    bool manual);

static void h2_loader_ble_link_task(void *ctx) {
    h2_loader_ble_service_t *service = ctx;
    for (;;) {
        uint16_t conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        (void)h2_pal_semaphore_take(
            service->config.api.sync,
            service->link_semaphore,
            H2_PAL_SYNC_WAIT_FOREVER);
        (void)h2_pal_mutex_lock(
            service->config.api.sync, service->link_mutex);
        if (service->closing) {
            (void)h2_pal_mutex_unlock(
                service->config.api.sync, service->link_mutex);
            break;
        }
        bool restart_advertising = service->advertising_restart_pending;
        service->advertising_restart_pending = false;
        if (service->link_pending) {
            conn_handle = service->pending_conn_handle;
            service->link_pending = false;
        }
        (void)h2_pal_mutex_unlock(
            service->config.api.sync, service->link_mutex);
        int advertising_pause_request = atomic_exchange(
            &service->advertising_pause_request,
            H2_LOADER_BLE_ADV_PAUSE_REQUEST_NONE);
        if (advertising_pause_request !=
            H2_LOADER_BLE_ADV_PAUSE_REQUEST_NONE) {
            bool pause = advertising_pause_request ==
                H2_LOADER_BLE_ADV_PAUSE_REQUEST_PAUSE;
            int advertising_rc = pause
                ? h2_loader_ble_service_pause_advertising_reason(
                      service, false)
                : h2_loader_ble_service_resume_advertising_reason(
                      service, false);
            if (advertising_rc != H2_PAL_OK &&
                advertising_rc != H2_PAL_ERR_INVALID_STATE) {
                printf(
                    "H2_LOADER_BLE_ERROR stage=adv_coexistence code=%d "
                    "paused=%u\n",
                    advertising_rc,
                    pause ? 1u : 0u);
            }
        }
        if (restart_advertising &&
            h2_loader_ble_service_can_restart_advertising(service)) {
            (void)h2_pal_time_sleep_ms(
                service->config.api.time,
                H2_LOADER_BLE_ADV_RESTART_GRACE_MS);
            if (h2_loader_ble_service_can_restart_advertising(service)) {
                int advertising_rc =
                    h2_loader_ble_service_update_advertising(
                        service,
                        service->additional_services,
                        service->additional_service_count,
                        false);
                if (advertising_rc != H2_PAL_OK &&
                    advertising_rc != H2_PAL_ERR_INVALID_STATE &&
                    !h2_loader_ble_service_is_connected(service)) {
                    printf(
                        "H2_LOADER_BLE_ERROR stage=adv_restart code=%d\n",
                        advertising_rc);
                }
            }
        }
        if (conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
            continue;
        }

        const h2_pal_ble_connection_params_t params = {
            .interval_min_ms = H2_LOADER_BLE_LINK_INTERVAL_MS,
            .interval_max_ms = H2_LOADER_BLE_LINK_INTERVAL_MS,
            .latency = 0u,
            .supervision_timeout_ms =
                H2_LOADER_BLE_LINK_SUPERVISION_TIMEOUT_MS,
        };
        int interval_rc = h2_pal_ble_update_connection(
            service->config.api.ble, conn_handle, &params);
        (void)h2_pal_mutex_lock(
            service->config.api.sync, service->link_mutex);
        bool still_connected = !service->closing &&
            service->active_conn_handle == conn_handle;
        uint16_t att_mtu = service->active_att_mtu;
        bool mtu_confirmed = service->active_mtu_confirmed;
        (void)h2_pal_mutex_unlock(
            service->config.api.sync, service->link_mutex);
        if (still_connected && !mtu_confirmed) {
            (void)h2_pal_semaphore_take(
                service->config.api.sync,
                service->mtu_semaphore,
                H2_LOADER_BLE_PEER_MTU_WAIT_MS);
            (void)h2_pal_mutex_lock(
                service->config.api.sync, service->link_mutex);
            still_connected = !service->closing &&
                service->active_conn_handle == conn_handle;
            att_mtu = service->active_att_mtu;
            mtu_confirmed = service->active_mtu_confirmed;
            (void)h2_pal_mutex_unlock(
                service->config.api.sync, service->link_mutex);
        }
        int mtu_rc = still_connected && !mtu_confirmed
            ? h2_pal_ble_exchange_mtu(
                  service->config.api.ble,
                  conn_handle,
                  &att_mtu,
                  H2_LOADER_BLE_LINK_TIMEOUT_MS)
            : still_connected ? H2_PAL_OK : H2_PAL_ERR_CLOSED;
        (void)h2_pal_mutex_lock(
            service->config.api.sync, service->link_mutex);
        still_connected = !service->closing &&
            service->active_conn_handle == conn_handle;
        (void)h2_pal_mutex_unlock(
            service->config.api.sync, service->link_mutex);
        int phy_rc = still_connected
            ? h2_pal_ble_set_preferred_phy(
                  service->config.api.ble,
                  conn_handle,
                  H2_PAL_BLE_PHY_2M,
                  H2_PAL_BLE_PHY_2M,
                  H2_LOADER_BLE_LINK_TIMEOUT_MS)
            : H2_PAL_ERR_CLOSED;
        printf(
            "H2_LOADER_BLE_LINK conn=%u interval_ms=%u interval_rc=%d "
            "att_mtu=%u mtu_rc=%d phy=2m phy_rc=%d\n",
            (unsigned)conn_handle,
            (unsigned)H2_LOADER_BLE_LINK_INTERVAL_MS,
            interval_rc,
            (unsigned)att_mtu,
            mtu_rc,
            phy_rc);
    }
}

static int h2_loader_ble_system_event(
    void *user,
    const h2_pal_system_event_t *event) {
    h2_loader_ble_service_t *service = user;
    if (service == NULL || event == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED &&
        event->payload_size == sizeof(h2_pal_ble_connection_t)) {
        const h2_pal_ble_connection_t *connection = event->payload;
        if (connection->role != H2_PAL_BLE_ROLE_PERIPHERAL) {
            return H2_PAL_OK;
        }
        (void)h2_pal_mutex_lock(
            service->config.api.sync, service->link_mutex);
        if (!service->closing) {
            service->active_conn_handle = connection->conn_handle;
            service->active_att_mtu = connection->mtu;
            service->active_mtu_confirmed = false;
            service->pending_conn_handle = connection->conn_handle;
            service->link_pending = true;
        }
        (void)h2_pal_mutex_unlock(
            service->config.api.sync, service->link_mutex);
        (void)h2_pal_semaphore_give(
            service->config.api.sync, service->link_semaphore);
        return H2_PAL_OK;
    }
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED &&
        event->payload_size == sizeof(h2_pal_ble_mtu_info_t)) {
        const h2_pal_ble_mtu_info_t *info = event->payload;
        (void)h2_pal_mutex_lock(
            service->config.api.sync, service->link_mutex);
        if (service->active_conn_handle == info->conn_handle) {
            service->active_att_mtu = info->mtu;
            service->active_mtu_confirmed = true;
        }
        (void)h2_pal_mutex_unlock(
            service->config.api.sync, service->link_mutex);
        (void)h2_pal_semaphore_give(
            service->config.api.sync, service->mtu_semaphore);
        return H2_PAL_OK;
    }
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED &&
        event->payload_size == sizeof(h2_pal_ble_disconnected_info_t)) {
        const h2_pal_ble_disconnected_info_t *info = event->payload;
        (void)h2_pal_mutex_lock(
            service->config.api.sync, service->link_mutex);
        if (service->link_pending &&
            service->pending_conn_handle == info->conn_handle) {
            service->link_pending = false;
            service->pending_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
        }
        if (service->active_conn_handle == info->conn_handle) {
            service->active_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
            service->active_att_mtu = 0u;
            service->active_mtu_confirmed = false;
        }
        service->advertising_restart_pending = !service->closing &&
            !service->advertising_paused;
        (void)h2_pal_mutex_unlock(
            service->config.api.sync, service->link_mutex);
        (void)h2_pal_semaphore_give(
            service->config.api.sync, service->link_semaphore);
        (void)h2_pal_semaphore_give(
            service->config.api.sync, service->mtu_semaphore);
        return H2_PAL_OK;
    }
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED) {
        if (service->config.advertising_mode ==
                H2_LOADER_BLE_ADVERTISING_EXTENDED) {
            if (event->payload_size != sizeof(h2_pal_ble_adv_set_event_t)) {
                return H2_PAL_OK;
            }
            const h2_pal_ble_adv_set_event_t *advertising = event->payload;
            if (advertising->set != service->adv_set) {
                return H2_PAL_OK;
            }
        } else if (event->payload_size != 0u) {
            return H2_PAL_OK;
        }
        (void)h2_pal_mutex_lock(
            service->config.api.sync, service->link_mutex);
        if (!service->closing && !service->advertising_paused &&
            service->active_conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
            service->advertising_restart_pending = true;
        }
        (void)h2_pal_mutex_unlock(
            service->config.api.sync, service->link_mutex);
        (void)h2_pal_semaphore_give(
            service->config.api.sync, service->link_semaphore);
        return H2_PAL_OK;
    }
    return H2_PAL_OK;
}

static int h2_loader_ble_start_link_task(h2_loader_ble_service_t *service) {
    h2_pal_mutex_config_t mutex_config = {
        .name = h2_loader_ble_link_task_name,
        .allocator = service->config.api.allocator,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    int rc = h2_pal_mutex_create(
        service->config.api.sync, &mutex_config, &service->link_mutex);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_pal_semaphore_config_t semaphore_config = {
        .name = h2_loader_ble_link_task_name,
        .allocator = service->config.api.allocator,
        .initial_count = 0u,
        .max_count = 1u,
    };
    rc = h2_pal_semaphore_create(
        service->config.api.sync,
        &semaphore_config,
        &service->link_semaphore);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    semaphore_config.name = "h2loader/blemtu";
    rc = h2_pal_semaphore_create(
        service->config.api.sync,
        &semaphore_config,
        &service->mtu_semaphore);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const h2_pal_task_options_t task_options = {
        .name = h2_loader_ble_link_task_name,
        .min_stack_size = H2_LOADER_BLE_LINK_TASK_STACK_SIZE,
    };
    return h2_pal_task_start(
        service->config.api.task,
        &task_options,
        h2_loader_ble_link_task,
        service,
        &service->link_task);
}

static int h2_loader_ble_stop_link_task(h2_loader_ble_service_t *service) {
    if (service->link_task != NULL) {
        (void)h2_pal_mutex_lock(
            service->config.api.sync, service->link_mutex);
        service->closing = true;
        atomic_store(&service->closing_requested, true);
        service->link_pending = false;
        (void)h2_pal_mutex_unlock(
            service->config.api.sync, service->link_mutex);
        (void)h2_pal_semaphore_give(
            service->config.api.sync, service->link_semaphore);
        (void)h2_pal_semaphore_give(
            service->config.api.sync, service->mtu_semaphore);
        int rc = h2_pal_task_join(
            service->config.api.task, service->link_task);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        service->link_task = NULL;
    }
    if (service->link_semaphore != NULL) {
        (void)h2_pal_semaphore_destroy(
            service->config.api.sync, service->link_semaphore);
        service->link_semaphore = NULL;
    }
    if (service->mtu_semaphore != NULL) {
        (void)h2_pal_semaphore_destroy(
            service->config.api.sync, service->mtu_semaphore);
        service->mtu_semaphore = NULL;
    }
    if (service->link_mutex != NULL) {
        (void)h2_pal_mutex_destroy(
            service->config.api.sync, service->link_mutex);
        service->link_mutex = NULL;
    }
    return H2_PAL_OK;
}

static void write_le32(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void write_le64(uint8_t out[8], uint64_t value) {
    for (size_t i = 0u; i < 8u; ++i) {
        out[i] = (uint8_t)(value >> (i * 8u));
    }
}

static uint64_t h2_loader_ble_board_hash(const char *board) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const uint8_t *cursor = (const uint8_t *)board;
         *cursor != 0u;
         ++cursor) {
        hash ^= *cursor;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

int h2_loader_ble_encode_identity(
    uint32_t capabilities,
    const char *board,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_len) {
    if (board == NULL || out == NULL || out_len == NULL ||
        (capabilities & ~H2_LOADER_CAPABILITIES_ALL) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t board_len = strlen(board);
    if (board_len == 0u || board_len > H2_LOADER_BLE_BOARD_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t required_capacity = board_len > H2_LOADER_BLE_INLINE_BOARD_MAX
        ? H2_LOADER_BLE_COMPACT_SERVICE_DATA_LEN
        : H2_LOADER_BLE_SERVICE_DATA_FIXED_LEN + board_len;
    if (out_capacity < required_capacity) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(out, "H2LD", 4u);
    out[5] = 0u;
    write_le32(&out[6], capabilities);
    if (board_len > H2_LOADER_BLE_INLINE_BOARD_MAX) {
        out[4] = H2_LOADER_BLE_COMPACT_PROTOCOL_VERSION;
        write_le64(&out[10], h2_loader_ble_board_hash(board));
        *out_len = H2_LOADER_BLE_COMPACT_SERVICE_DATA_LEN;
        return H2_PAL_OK;
    }
    out[4] = H2_LOADER_BLE_PROTOCOL_VERSION;
    out[10] = (uint8_t)board_len;
    memcpy(&out[11], board, board_len);
    *out_len = H2_LOADER_BLE_SERVICE_DATA_FIXED_LEN + board_len;
    return H2_PAL_OK;
}

static h2_pal_result_t command_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    return (h2_pal_result_t)h2_bleikcp_read(
        user, buffer, len, out_read, timeout_ms);
}

static h2_pal_result_t command_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    int rc;
    if (out_written == NULL || (len > 0u && buffer == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_written = 0u;
    rc = h2_bleikcp_write(user, buffer, len, timeout_ms);
    if (rc == H2_PAL_OK) {
        *out_written = len;
    }
    return rc;
}

static h2_pal_result_t command_flush(void *user) {
    return (h2_pal_result_t)h2_bleikcp_flush(
        user, H2_LOADER_BLE_IO_TIMEOUT_MS);
}

h2_command_io_api_t h2_loader_ble_command_io(h2_bleikcp_t *stream) {
    static const h2_command_io_vtable_t vtable = {
        .read = command_read,
        .write = command_write,
        .flush = command_flush,
    };
    return (h2_command_io_api_t){
        .user = stream,
        .vtable = stream != NULL ? &vtable : NULL,
    };
}

int h2_loader_ble_app_read_byte(void *user, uint32_t timeout_ms) {
    uint8_t value = 0u;
    size_t count = 0u;
    int rc = h2_bleikcp_read(user, &value, sizeof(value), &count, timeout_ms);
    if (rc == H2_PAL_OK && count == 1u) {
        return value;
    }
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK ||
        (rc == H2_PAL_OK && count == 0u)) {
        return EOF;
    }
    return H2_LOADER_APP_CLIENT_SESSION_CLOSED;
}

int h2_loader_ble_app_write(void *user, const char *data, size_t len) {
    int rc;
    if (data == NULL && len > 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = len > 0u
        ? h2_bleikcp_write(user, (const uint8_t *)data, len,
              H2_LOADER_BLE_IO_TIMEOUT_MS)
        : H2_PAL_OK;
    return rc == H2_PAL_OK
        ? h2_bleikcp_flush(user, H2_LOADER_BLE_IO_TIMEOUT_MS)
        : rc;
}

int h2_loader_ble_service_open(
    const h2_loader_ble_service_config_t *config,
    h2_loader_ble_service_t **out_service) {
    h2_loader_ble_service_t *service;
    h2_bleikcp_config_t stream_config;
    h2_pal_ble_adv_params_t adv_params;
    static const h2_pal_system_event_type_t event_types[
        H2_LOADER_BLE_EVENT_SUBSCRIPTION_COUNT] = {
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_CONNECTED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED,
        H2_PAL_SYSTEM_EVENT_TYPE_BLE_ADVERTISING_STOPPED,
    };
    int rc;

    if (config == NULL || out_service == NULL || config->api.ble == NULL ||
        config->api.task == NULL || config->api.time == NULL ||
        config->api.sync == NULL || config->api.system_event == NULL ||
        config->api.allocator == NULL || config->board == NULL ||
        config->handler == NULL ||
        (config->advertising_mode != H2_LOADER_BLE_ADVERTISING_LEGACY &&
         config->advertising_mode != H2_LOADER_BLE_ADVERTISING_EXTENDED)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_service = NULL;
    service = h2_pal_mem_alloc(config->api.allocator, sizeof(*service));
    if (service == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(service, 0, sizeof(*service));
    atomic_init(
        &service->advertising_pause_request,
        H2_LOADER_BLE_ADV_PAUSE_REQUEST_NONE);
    atomic_init(&service->closing_requested, false);
    service->config = *config;
    service->active_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    service->pending_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    rc = h2_loader_ble_encode_identity(
        config->capabilities, config->board,
        service->service_data, sizeof(service->service_data),
        &service->service_data_len);
    if (rc != H2_PAL_OK) {
        goto fail;
    }

    memset(&stream_config, 0, sizeof(stream_config));
    stream_config.service_uuid = (h2_pal_ble_uuid_t){
        .data = h2_loader_ble_service_uuid_bytes,
        .len = sizeof(h2_loader_ble_service_uuid_bytes),
    };
    stream_config.tx_char_uuid = (h2_pal_ble_uuid_t){
        .data = h2_loader_ble_tx_uuid_bytes,
        .len = sizeof(h2_loader_ble_tx_uuid_bytes),
    };
    stream_config.rx_char_uuid = (h2_pal_ble_uuid_t){
        .data = h2_loader_ble_rx_uuid_bytes,
        .len = sizeof(h2_loader_ble_rx_uuid_bytes),
    };
    stream_config.send_window = H2_LOADER_BLE_WINDOW;
    stream_config.recv_window = H2_LOADER_BLE_WINDOW;
    stream_config.input_frame_capacity = H2_LOADER_BLE_INPUT_FRAME_CAPACITY;
    stream_config.tx_buffer_size = H2_LOADER_BLE_BUFFER_SIZE;
    stream_config.rx_buffer_size = H2_LOADER_BLE_BUFFER_SIZE;
    stream_config.no_congestion_control = 0;
    stream_config.output_retry_count = 40u;
    stream_config.output_retry_delay_ms = 2u;
    stream_config.on_event = h2_loader_ble_stream_event;
    rc = h2_bleikcp_server_open(
        &service->config.api, &stream_config, config->handler,
        config->handler_user, &service->server);
    if (rc != H2_PAL_OK) {
        rc = h2_loader_ble_open_error("server_open", rc);
        goto fail;
    }
    rc = h2_pal_ble_start(config->api.ble);
    if (rc != H2_PAL_OK) {
        rc = h2_loader_ble_open_error("host_start", rc);
        goto fail;
    }
    rc = h2_loader_ble_start_link_task(service);
    if (rc != H2_PAL_OK) {
        rc = h2_loader_ble_open_error("link_task_start", rc);
        goto fail;
    }

    adv_params = (h2_pal_ble_adv_params_t){
        .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
        .type = config->advertising_mode ==
                H2_LOADER_BLE_ADVERTISING_LEGACY ?
            H2_PAL_BLE_ADV_TYPE_LEGACY :
            H2_PAL_BLE_ADV_TYPE_EXTENDED,
        .interval_min_ms = H2_LOADER_BLE_ADV_INTERVAL_MIN_MS,
        .interval_max_ms = H2_LOADER_BLE_ADV_INTERVAL_MAX_MS,
        .primary_phy = H2_PAL_BLE_PHY_1M,
        .secondary_phy = config->advertising_mode ==
                H2_LOADER_BLE_ADVERTISING_LEGACY ?
            H2_PAL_BLE_PHY_1M :
            H2_PAL_BLE_PHY_2M,
        .sid = config->advertising_mode ==
                H2_LOADER_BLE_ADVERTISING_LEGACY ?
            0u :
            H2_LOADER_BLE_ADV_SID,
    };
    service->adv_params = adv_params;
    if (config->advertising_mode == H2_LOADER_BLE_ADVERTISING_EXTENDED) {
        rc = h2_pal_ble_adv_set_create(
            config->api.ble, &adv_params, &service->adv_set);
        if (rc != H2_PAL_OK) {
            rc = h2_loader_ble_open_error("adv_create", rc);
            goto fail;
        }
    }
    for (size_t i = 0u; i < H2_LOADER_BLE_EVENT_SUBSCRIPTION_COUNT; ++i) {
        rc = h2_pal_system_event_subscribe(
            config->api.system_event,
            event_types[i],
            h2_loader_ble_system_event,
            service,
            &service->event_subscriptions[i]);
        if (rc != H2_PAL_OK) {
            rc = h2_loader_ble_open_error("event_subscribe", rc);
            goto fail;
        }
    }
    rc = h2_loader_ble_service_update_advertising(
        service, NULL, 0u, false);
    if (rc != H2_PAL_OK) {
        rc = h2_loader_ble_open_error("adv_data_or_start", rc);
        goto fail;
    }
    *out_service = service;
    return H2_PAL_OK;

fail:
    for (size_t i = 0u; i < H2_LOADER_BLE_EVENT_SUBSCRIPTION_COUNT; ++i) {
        if (service->event_subscriptions[i] != NULL) {
            (void)h2_pal_system_event_unsubscribe(
                config->api.system_event, service->event_subscriptions[i]);
        }
    }
    int link_rc = h2_loader_ble_stop_link_task(service);
    if (link_rc != H2_PAL_OK) {
        return h2_loader_ble_open_error("link_task_stop", link_rc);
    }
    if (service->adv_set != NULL) {
        (void)h2_pal_ble_adv_set_destroy(config->api.ble, service->adv_set);
    } else if (service->config.advertising_mode ==
               H2_LOADER_BLE_ADVERTISING_LEGACY) {
        (void)h2_pal_ble_stop_advertising(config->api.ble);
    }
    if (service->server != NULL) {
        (void)h2_bleikcp_server_close(service->server);
    }
    h2_pal_mem_free(config->api.allocator, service);
    return rc;
}

int h2_loader_ble_service_close(h2_loader_ble_service_t *service) {
    int adv_rc;
    int server_rc;
    if (service == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < H2_LOADER_BLE_EVENT_SUBSCRIPTION_COUNT; ++i) {
        if (service->event_subscriptions[i] != NULL) {
            (void)h2_pal_system_event_unsubscribe(
                service->config.api.system_event,
                service->event_subscriptions[i]);
            service->event_subscriptions[i] = NULL;
        }
    }
    int link_rc = h2_loader_ble_stop_link_task(service);
    if (link_rc != H2_PAL_OK) {
        return link_rc;
    }
    adv_rc = service->config.advertising_mode ==
            H2_LOADER_BLE_ADVERTISING_LEGACY
        ? h2_pal_ble_stop_advertising(service->config.api.ble)
        : service->adv_set != NULL
            ? h2_pal_ble_adv_set_destroy(
                  service->config.api.ble, service->adv_set)
            : H2_PAL_OK;
    if (adv_rc == H2_PAL_ERR_INVALID_STATE &&
        service->config.advertising_mode ==
            H2_LOADER_BLE_ADVERTISING_LEGACY) {
        adv_rc = H2_PAL_OK;
    }
    server_rc = service->server != NULL
        ? h2_bleikcp_server_close(service->server)
        : H2_PAL_OK;
    const h2_pal_mem_api_t *allocator = service->config.api.allocator;
    h2_pal_mem_free(allocator, service);
    return adv_rc != H2_PAL_OK ? adv_rc : server_rc;
}

static int h2_loader_ble_service_pause_advertising_reason(
    h2_loader_ble_service_t *service,
    bool manual) {
    if (service == NULL ||
        (service->config.advertising_mode ==
             H2_LOADER_BLE_ADVERTISING_EXTENDED &&
         service->adv_set == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)h2_pal_mutex_lock(
        service->config.api.sync, service->link_mutex);
    if (manual) {
        service->advertising_pause_manual = true;
    } else {
        service->advertising_pause_auto = true;
    }
    service->advertising_paused = true;
    service->advertising_restart_pending = false;
    (void)h2_pal_mutex_unlock(
        service->config.api.sync, service->link_mutex);
    int rc = service->config.advertising_mode ==
            H2_LOADER_BLE_ADVERTISING_LEGACY
        ? h2_pal_ble_stop_advertising(service->config.api.ble)
        : h2_pal_ble_adv_set_stop(
              service->config.api.ble, service->adv_set);
    if (rc == H2_PAL_OK || rc == H2_PAL_ERR_INVALID_STATE) {
        return H2_PAL_OK;
    }
    (void)h2_pal_mutex_lock(
        service->config.api.sync, service->link_mutex);
    if (manual) {
        service->advertising_pause_manual = false;
    } else {
        service->advertising_pause_auto = false;
    }
    service->advertising_paused = service->advertising_pause_manual ||
        service->advertising_pause_auto;
    (void)h2_pal_mutex_unlock(
        service->config.api.sync, service->link_mutex);
    return rc;
}

int h2_loader_ble_service_pause_advertising(
    h2_loader_ble_service_t *service) {
    return h2_loader_ble_service_pause_advertising_reason(service, true);
}

static int h2_loader_ble_service_resume_advertising_reason(
    h2_loader_ble_service_t *service,
    bool manual) {
    if (service == NULL ||
        (service->config.advertising_mode ==
             H2_LOADER_BLE_ADVERTISING_EXTENDED &&
         service->adv_set == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)h2_pal_mutex_lock(
        service->config.api.sync, service->link_mutex);
    if (manual) {
        service->advertising_pause_manual = false;
    } else {
        service->advertising_pause_auto = false;
    }
    /* The other reason still owns the pause: drop this one and leave
     * advertising stopped instead of overriding it. */
    bool still_paused = service->advertising_pause_manual ||
        service->advertising_pause_auto;
    service->advertising_paused = still_paused;
    (void)h2_pal_mutex_unlock(
        service->config.api.sync, service->link_mutex);
    if (still_paused) {
        return H2_PAL_OK;
    }
    int rc = H2_PAL_ERR_WOULD_BLOCK;
    for (uint32_t attempt = 0u;
         attempt < H2_LOADER_BLE_ADV_RESUME_RETRY_COUNT;
         ++attempt) {
        rc = service->config.advertising_mode ==
                H2_LOADER_BLE_ADVERTISING_LEGACY
            ? h2_pal_ble_start_advertising(
                  service->config.api.ble, &service->adv_params)
            : h2_pal_ble_adv_set_start(
                  service->config.api.ble, service->adv_set);
        if (rc == H2_PAL_OK || rc == H2_PAL_ERR_INVALID_STATE) {
            return H2_PAL_OK;
        }
        if (rc != H2_PAL_ERR_WOULD_BLOCK) {
            break;
        }
        (void)h2_pal_time_sleep_ms(
            service->config.api.time,
            H2_LOADER_BLE_ADV_RESUME_RETRY_DELAY_MS);
    }
    (void)h2_pal_mutex_lock(
        service->config.api.sync, service->link_mutex);
    service->advertising_paused = true;
    (void)h2_pal_mutex_unlock(
        service->config.api.sync, service->link_mutex);
    return rc;
}

int h2_loader_ble_service_resume_advertising(
    h2_loader_ble_service_t *service) {
    return h2_loader_ble_service_resume_advertising_reason(service, true);
}

int h2_loader_ble_service_request_advertising_paused(
    h2_loader_ble_service_t *service,
    bool paused) {
    if (service == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (atomic_load(&service->closing_requested)) {
        return H2_PAL_ERR_CLOSED;
    }
    atomic_store(
        &service->advertising_pause_request,
        paused ? H2_LOADER_BLE_ADV_PAUSE_REQUEST_PAUSE
               : H2_LOADER_BLE_ADV_PAUSE_REQUEST_RESUME);
    return h2_pal_semaphore_give(
        service->config.api.sync, service->link_semaphore);
}

int h2_loader_ble_service_set_additional_advertised_services(
    h2_loader_ble_service_t *service,
    const h2_pal_ble_uuid_t *services,
    size_t service_count) {
    if (service == NULL ||
        service_count > H2_LOADER_BLE_MAX_ADDITIONAL_ADVERTISED_SERVICES ||
        (service_count > 0u && services == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < service_count; ++i) {
        if (services[i].data == NULL ||
            (services[i].len != 2u && services[i].len != 4u &&
             services[i].len != 16u)) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    (void)h2_pal_mutex_lock(
        service->config.api.sync, service->link_mutex);
    service->additional_service_count = service_count;
    for (size_t i = 0u; i < service_count; ++i) {
        memcpy(
            service->additional_service_data[i],
            services[i].data,
            services[i].len);
        service->additional_services[i].data =
            service->additional_service_data[i];
        service->additional_services[i].len = services[i].len;
    }
    bool connected = service->active_conn_handle !=
        H2_PAL_BLE_INVALID_CONN_HANDLE;
    (void)h2_pal_mutex_unlock(
        service->config.api.sync, service->link_mutex);
    if (connected) {
        return H2_PAL_OK;
    }
    int rc = h2_loader_ble_service_update_advertising(
        service,
        service->additional_services,
        service->additional_service_count,
        true);
    if (rc != H2_PAL_OK &&
        h2_loader_ble_service_is_connected(service)) {
        return H2_PAL_OK;
    }
    return rc;
}
