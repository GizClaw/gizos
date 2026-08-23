#include "h2_bk3633_platform_core.h"
#include "h2_bk3633_ble_sdk_fake.h"
#include "h2_bk3633_mem_test_support.h"
#include "h2_libco.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_write_calls;
static unsigned s_read_calls;
static uint64_t s_now_ms;
static bool s_mem_alloc_should_fail;

typedef struct indication_call {
    const h2_pal_ble_host_api_t *api;
    uint16_t value_handle;
    uint32_t timeout_ms;
    h2_pal_result_t result;
} indication_call_t;

static void *test_alloc(void *user, size_t size) {
    (void)user;
    if (s_mem_alloc_should_fail) {
        return NULL;
    }
    return malloc(size);
}

static void test_free(void *user, void *memory) {
    (void)user;
    free(memory);
}

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = test_alloc,
    .free = test_free,
};

static const h2_pal_mem_api_t s_mem_api = {
    .vtable = &s_mem_vtable,
};

static uint64_t test_now(void *user) {
    (void)user;
    return s_now_ms;
}

static h2_pal_result_t time_now(void *user, uint64_t *out_ms) {
    (void)user;
    *out_ms = s_now_ms;
    return H2_PAL_OK;
}

static int indication_entry(void *user) {
    indication_call_t *call = user;
    static const uint8_t payload[] = {0x11u, 0x22u};
    call->result = h2_pal_ble_indicate(
        call->api, 22u, call->value_handle,
        payload, sizeof(payload), call->timeout_ms);
    return 0;
}

static h2_libco_task_t *start_indication(
    h2_libco_t *executor, indication_call_t *call) {
    h2_libco_task_t *task = NULL;
    size_t resumed = 0u;
    assert(h2_libco_task_start(
               executor, NULL, indication_entry, call, &task) == H2_LIBCO_OK);
    assert(h2_libco_schedule(executor, 1u, &resumed) == H2_LIBCO_OK);
    assert(resumed == 1u);
    assert(h2_libco_task_join(executor, task, NULL) == H2_LIBCO_ERR_BUSY);
    return task;
}

static void finish_indication(h2_libco_t *executor,
                              h2_libco_task_t *task,
                              h2_pal_result_t expected) {
    size_t resumed = 0u;
    assert(h2_bk3633_platform_libco_dispatch_wakes(
               SIZE_MAX, NULL) == H2_BK3633_PLATFORM_LIBCO_OK);
    assert(h2_libco_schedule(executor, 1u, &resumed) == H2_LIBCO_OK);
    assert(resumed == 1u);
    assert(h2_libco_task_join(executor, task, NULL) == H2_LIBCO_OK);
    (void)expected;
}

static h2_pal_result_t write_value(
    void *user, const h2_pal_ble_gatt_access_t *access,
    const uint8_t *data, size_t len) {
    (void)user;
    (void)access;
    assert(len == 1u && data[0] == 0x5au);
    ++s_write_calls;
    return H2_PAL_OK;
}

static h2_pal_result_t read_value(
    void *user, const h2_pal_ble_gatt_access_t *access,
    uint8_t *out, size_t out_size, size_t *out_len) {
    (void)user;
    (void)access;
    assert(out_size >= 2u);
    ++s_read_calls;
    out[0] = 0x11u;
    out[1] = 0x22u;
    *out_len = 2u;
    return H2_PAL_OK;
}

static struct gattc_write_req_ind *write_request(
    uint8_t storage[sizeof(struct gattc_write_req_ind) + 2u],
    uint16_t handle, uint8_t value) {
    memset(storage, 0, sizeof(struct gattc_write_req_ind) + 2u);
    struct gattc_write_req_ind *request =
        (struct gattc_write_req_ind *)storage;
    request->handle = handle;
    request->length = 1u;
    request->value[0] = value;
    return request;
}

int main(void) {
    h2_bk3633_mem_test_support_init();
    h2_libco_t *executor = NULL;
    const h2_libco_config_t executor_config = {
        .alloc = test_alloc,
        .free = test_free,
        .now_ms = test_now,
    };
    static const h2_pal_time_vtable_t time_vtable = {
        .get_monotonic_ms = time_now,
    };
    static const h2_pal_time_api_t time_api = {
        .user = NULL,
        .vtable = &time_vtable,
    };
    h2_bk3633_platform_ble_config_t ble_config = {
        .mem = &s_mem_api,
        .time = &time_api,
        .gatt_pending_access_capacity = 2u,
        .bootstrap_timeout_ms = 100u,
    };

    h2_bk3633_ble_sdk_fake_reset();
    h2_bk3633_platform_ble_test_reset();
    assert(h2_bk3633_platform_ble_configure(NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    ble_config.gatt_pending_access_capacity = 0u;
    assert(h2_bk3633_platform_ble_configure(&ble_config) ==
           H2_PAL_ERR_INVALID_ARG);
    ble_config.gatt_pending_access_capacity = SIZE_MAX;
    assert(h2_bk3633_platform_ble_configure(&ble_config) ==
           H2_PAL_ERR_INVALID_ARG);
    ble_config.gatt_pending_access_capacity = 2u;
    s_mem_alloc_should_fail = true;
    assert(h2_bk3633_platform_ble_configure(&ble_config) ==
           H2_PAL_ERR_NO_MEMORY);
    s_mem_alloc_should_fail = false;
    assert(h2_bk3633_platform_ble_configure(&ble_config) == H2_PAL_OK);
    assert(h2_bk3633_platform_ble_configure(&ble_config) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_libco_create(&executor_config, &executor) == H2_LIBCO_OK);
    const h2_bk3633_platform_libco_config_t completion_config = {
        .executor = executor,
        .allocator = &s_mem_api,
        .completion_capacity = 3u,
    };
    assert(h2_bk3633_platform_libco_bind(&completion_config) ==
           H2_BK3633_PLATFORM_LIBCO_OK);
    assert(h2_pal_system_event_init(
               h2_bk3633_platform_system_event_api()) == H2_PAL_OK);
    assert(h2_bk3633_platform_ble_host_bootstrap_begin() == H2_PAL_OK);
    h2_bk3633_platform_ble_host_bootstrap_complete(H2_PAL_OK);
    const h2_pal_ble_host_api_t *api = h2_bk3633_platform_ble_api();
    assert(h2_pal_ble_start(api) == H2_PAL_OK);
    const uint8_t service_uuid[] = {0xe0u, 0xfeu};
    const uint8_t value_uuid[] = {0xe3u, 0xfeu};
    uint16_t value_handle = 0u;
    uint16_t cccd_handle = 0u;
    const h2_pal_ble_gatt_characteristic_t characteristic = {
        .uuid = {.data = value_uuid, .len = sizeof(value_uuid)},
        .properties = H2_PAL_BLE_GATT_PROPERTY_WRITE |
                      H2_PAL_BLE_GATT_PROPERTY_READ |
                      H2_PAL_BLE_GATT_PROPERTY_NOTIFY |
                      H2_PAL_BLE_GATT_PROPERTY_INDICATE,
        .permissions = H2_PAL_BLE_GATT_PERMISSION_READ |
                       H2_PAL_BLE_GATT_PERMISSION_WRITE,
        .max_value_len = 16u,
        .read = read_value,
        .write = write_value,
        .out_value_handle = &value_handle,
        .out_cccd_handle = &cccd_handle,
    };
    const h2_pal_ble_gatt_service_t service = {
        .uuid = {.data = service_uuid, .len = sizeof(service_uuid)},
        .primary = true,
        .characteristics = &characteristic,
        .characteristic_count = 1u,
    };
    assert(h2_pal_ble_register_gatt_services(api, &service, 1u) == H2_PAL_OK);
    const struct gapc_connection_req_ind connection = {
        .conhdl = 22u,
        .role = 1u,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPC_CONNECTION_REQ_IND, &connection, TASK_APP,
        KE_BUILD_ID(TASK_GAPC, 1u));

    uint8_t cccd_storage[sizeof(struct gattc_write_req_ind) + 2u] = {0};
    struct gattc_write_req_ind *cccd =
        (struct gattc_write_req_ind *)cccd_storage;
    cccd->handle = cccd_handle;
    cccd->length = 2u;
    cccd->value[0] = 0x03u;
    cccd->value[1] = 0x00u;
    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_WRITE_REQ_IND, cccd, TASK_APP,
        KE_BUILD_ID(TASK_GATTC, 1u));

    size_t rejection_start = h2_bk3633_ble_sdk_fake_message_count();
    uint8_t queued_storage[5][sizeof(struct gattc_write_req_ind) + 2u];
    for (size_t i = 0u; i < 5u; ++i) {
        struct gattc_write_req_ind *request = write_request(
            queued_storage[i], value_handle, 0x5au);
        (void)h2_bk3633_platform_ble_dispatch(
            GATTC_WRITE_REQ_IND, request, TASK_APP,
            KE_BUILD_ID(TASK_GATTC, 1u));
    }
    assert(h2_bk3633_ble_sdk_fake_message_count() == rejection_start + 3u);
    for (size_t i = rejection_start; i < rejection_start + 3u; ++i) {
        const h2_bk3633_ble_sdk_fake_message_t *message =
            h2_bk3633_ble_sdk_fake_message(i);
        assert(message != NULL && message->id == GATTC_WRITE_CFM);
        const struct gattc_write_cfm *confirmation = message->payload;
        assert(confirmation->status == ATT_ERR_INSUFF_RESOURCE);
    }
    assert(s_write_calls == 0u);
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(s_write_calls == 2u);
    assert(h2_bk3633_ble_sdk_fake_message_count() == rejection_start + 5u);
    for (size_t i = rejection_start + 3u;
         i < rejection_start + 5u; ++i) {
        const h2_bk3633_ble_sdk_fake_message_t *message =
            h2_bk3633_ble_sdk_fake_message(i);
        assert(message != NULL && message->id == GATTC_WRITE_CFM);
        const struct gattc_write_cfm *confirmation = message->payload;
        assert(confirmation->status == ATT_ERR_NO_ERROR);
    }

    uint8_t retry_storage[sizeof(struct gattc_write_req_ind) + 2u];
    struct gattc_write_req_ind *retry = write_request(
        retry_storage, value_handle, 0x5au);
    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_WRITE_REQ_IND, retry, TASK_APP,
        KE_BUILD_ID(TASK_GATTC, 1u));
    h2_bk3633_ble_sdk_fake_fail_next_allocations(1u);
    assert(h2_bk3633_platform_ble_dispatch_pending() ==
           H2_PAL_ERR_NO_MEMORY);
    assert(s_write_calls == 3u);
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(s_write_calls == 3u);

    const struct gattc_read_req_ind read_request = {.handle = value_handle};
    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_READ_REQ_IND, &read_request, TASK_APP,
        KE_BUILD_ID(TASK_GATTC, 1u));
    h2_bk3633_ble_sdk_fake_fail_next_allocations(1u);
    assert(h2_bk3633_platform_ble_dispatch_pending() ==
           H2_PAL_ERR_NO_MEMORY);
    assert(s_read_calls == 1u);
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(s_read_calls == 1u);

    const uint8_t payload[] = {0x11u, 0x22u};
    assert(h2_pal_ble_notify(
               api, 22u, value_handle, payload, sizeof(payload)) == H2_PAL_OK);

    h2_bk3633_platform_ble_test_set_indication_sequence(0u);
    indication_call_t indication = {
        .api = api,
        .value_handle = value_handle,
        .timeout_ms = 10u,
        .result = H2_PAL_ERR_INVALID_STATE,
    };
    h2_libco_task_t *indication_task =
        start_indication(executor, &indication);
    const struct gattc_cmp_evt confirmed = {
        .operation = GATTC_INDICATE,
        .status = ATT_ERR_NO_ERROR,
        .seq_num = 1u,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_CMP_EVT, &confirmed, TASK_APP,
        KE_BUILD_ID(TASK_GATTC, 1u));
    finish_indication(executor, indication_task, H2_PAL_OK);
    assert(indication.result == H2_PAL_OK);

    indication.result = H2_PAL_ERR_INVALID_STATE;
    indication_task = start_indication(executor, &indication);
    const struct gattc_cmp_evt failed = {
        .operation = GATTC_INDICATE,
        .status = ATT_ERR_UNLIKELY_ERR,
        .seq_num = 2u,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_CMP_EVT, &failed, TASK_APP,
        KE_BUILD_ID(TASK_GATTC, 1u));
    finish_indication(executor, indication_task, H2_PAL_ERR_IO);
    assert(indication.result == H2_PAL_ERR_IO);

    indication.timeout_ms = 5u;
    indication.result = H2_PAL_ERR_INVALID_STATE;
    indication_task = start_indication(executor, &indication);
    s_now_ms = 5u;
    size_t resumed = 0u;
    assert(h2_libco_schedule(executor, 1u, &resumed) == H2_LIBCO_OK);
    assert(h2_libco_task_join(executor, indication_task, NULL) == H2_LIBCO_OK);
    assert(indication.result == H2_PAL_ERR_TIMEOUT);
    const struct gattc_cmp_evt late = {
        .operation = GATTC_INDICATE,
        .status = ATT_ERR_NO_ERROR,
        .seq_num = 3u,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_CMP_EVT, &late, TASK_APP, KE_BUILD_ID(TASK_GATTC, 1u));

    indication.timeout_ms = 10u;
    indication.result = H2_PAL_ERR_INVALID_STATE;
    indication_task = start_indication(executor, &indication);
    const struct gapc_disconnect_ind disconnected = {
        .conhdl = 22u,
        .reason = 0x13u,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPC_DISCONNECT_IND, &disconnected, TASK_APP,
        KE_BUILD_ID(TASK_GAPC, 1u));
    finish_indication(executor, indication_task, H2_PAL_ERR_CLOSED);
    assert(indication.result == H2_PAL_ERR_CLOSED);

    const h2_pal_ble_adv_params_t adv_params = {
        .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
        .interval_min_ms = 100u,
        .interval_max_ms = 120u,
        .type = H2_PAL_BLE_ADV_TYPE_LEGACY,
        .primary_phy = H2_PAL_BLE_PHY_1M,
        .secondary_phy = H2_PAL_BLE_PHY_1M,
    };
    h2_pal_ble_adv_set_t *first_set = NULL;
    h2_pal_ble_adv_set_t *second_set = NULL;
    assert(h2_pal_ble_adv_set_create(api, &adv_params, &first_set) ==
           H2_PAL_OK);
    assert(h2_pal_ble_adv_set_create(api, &adv_params, &second_set) ==
           H2_PAL_OK);

    const struct gapc_connection_req_ind stop_connection = {
        .conhdl = 23u,
        .role = 1u,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPC_CONNECTION_REQ_IND, &stop_connection, TASK_APP,
        KE_BUILD_ID(TASK_GAPC, 1u));
    uint8_t stop_storage[sizeof(struct gattc_write_req_ind) + 2u];
    struct gattc_write_req_ind *stop_request = write_request(
        stop_storage, value_handle, 0x5au);
    (void)h2_bk3633_platform_ble_dispatch(
        GATTC_WRITE_REQ_IND, stop_request, TASK_APP,
        KE_BUILD_ID(TASK_GATTC, 1u));
    h2_bk3633_ble_sdk_fake_fail_next_allocations(1u);
    assert(h2_pal_ble_stop(api) == H2_PAL_ERR_NO_MEMORY);
    assert(s_write_calls == 3u);
    assert(h2_pal_ble_stop(api) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_destroy(api, first_set) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_ble_adv_set_destroy(api, second_set) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_ble_start(api) == H2_PAL_ERR_INVALID_STATE);
    const struct gapm_cmp_evt reset_complete = {
        .operation = GAPM_RESET,
        .status = GAP_ERR_NO_ERROR,
    };
    (void)h2_bk3633_platform_ble_dispatch(
        GAPM_CMP_EVT, &reset_complete, TASK_APP, TASK_GAPM);
    assert(h2_pal_ble_start(api) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_bk3633_platform_ble_dispatch_pending() == H2_PAL_OK);
    assert(h2_pal_ble_start(api) == H2_PAL_OK);
    assert(h2_pal_ble_register_gatt_services(api, &service, 1u) == H2_PAL_OK);
    h2_bk3633_platform_ble_test_reset();
    h2_bk3633_platform_libco_unbind();
    assert(h2_libco_destroy(&executor) == H2_LIBCO_OK);
    return 0;
}
