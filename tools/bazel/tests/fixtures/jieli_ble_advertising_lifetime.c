#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/os/h2_pal_system_event.h"
enum { H2_JIELI_ADV_DATA_MAX = 251, ADV_SET_2M_PHY = 2, ADV_SET_CODED_PHY = 3,
    ADV_SET_1M_PHY = 1, ADV_IND = 0, ADV_NONCONN_IND = 3, ADV_CHANNEL_ALL = 7 };
/* REAL_TYPES */
static struct {
    int started, starting, stopping;
    uint16_t conn_handle;
    unsigned conn_pending, conn_submitting, conn_hook_skipped;
    uint32_t conn_generation;
    struct h2_pal_ble_adv_set adv;
} h2_ble;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static void h2_gatt_lock(void) { assert(pthread_mutex_lock(&gate) == 0); }
static void h2_gatt_unlock(void) { assert(pthread_mutex_unlock(&gate) == 0); }
static void check_unlocked(void) { h2_gatt_lock(); h2_gatt_unlock(); }
static void h2_restart_legacy_advertising(void);
static void (*sdk_hook)(void);
static int (*controller_fence)(int);
static int controller_generation;
static int early_hook, registrations, stop_hook;
static int fence_error, registration_error, stop_error, sdk_starts, posted;
static atomic_int hold_submit, submit_entered, release_submit;
enum { Q_CALLBACK = 0x300000 };
int ble_op_regist_thread_call(void (*hook)(void)) {
    check_unlocked();
    if (registration_error)
        return registration_error;
    ++registrations;
    sdk_hook = hook;
    if (early_hook && registrations == 1) {
        sdk_hook = NULL;
        hook();
        assert(h2_adv_commands.phase == 1u);
        assert(controller_fence == NULL);
    }
    return 0;
}
int ble_cmd_handler_is_idle(void) { check_unlocked(); return 1; }
void stack_run_loop_resume(void) { check_unlocked(); }
int btctrler_hci_cmd_to_task(int type, int count, ...) {
    check_unlocked();
    assert(type == Q_CALLBACK && count == 3);
    if (fence_error)
        return fence_error;
    va_list args;
    va_start(args, count);
    controller_fence = va_arg(args, int (*)(int));
    assert(va_arg(args, int) == 1);
    controller_generation = va_arg(args, int);
    va_end(args);
    return 0;
}
static const uint8_t *legacy_data;
static const struct h2_ext_adv_param *ext_params;
static const struct h2_ext_adv_data *ext_data;
static const struct h2_ext_adv_enable *ext_enable;
static void h2_ble_log(const char *format, ...) { (void)format; check_unlocked(); }
static int h2_ble_cmd_result(int value) { return value; }
static int h2_ble_cmd_trace(const char *name, int value) { (void)name; return value; }
static int ble_op_set_adv_param(uint16_t interval, int mode, int channels) {
    check_unlocked();
    ++sdk_starts;
    assert(interval != 0 && (mode == ADV_IND || mode == ADV_NONCONN_IND) && channels == 7);
    return 0;
}
static int ble_op_set_adv_data(uint8_t size, const uint8_t *data) {
    check_unlocked();
    assert(size > 0);
    legacy_data = data;
    return 0;
}
static int ble_op_set_rsp_data(uint8_t size, const uint8_t *data) { (void)size; (void)data; return 0; }
static int ble_op_adv_enable(int enabled) {
    check_unlocked();
    if (!enabled && stop_hook) {
        void (*hook)(void) = sdk_hook;
        sdk_hook = NULL;
        assert(hook != NULL);
        hook();
    }
    return enabled ? 0 : stop_error;
}
static int ble_op_set_ext_adv_param(const void *data, uint16_t size) {
    check_unlocked();
    assert(size == sizeof(*ext_params));
    ext_params = data;
    ++sdk_starts;
    if (atomic_load(&hold_submit) && sdk_starts == 1) {
        atomic_store(&submit_entered, 1);
        while (!atomic_load(&release_submit))
            sched_yield();
    }
    return 0;
}
static int ble_op_set_ext_adv_data(const void *data, uint16_t size) {
    check_unlocked();
    ext_data = data;
    assert(size == 4 + ext_data->length);
    return 0;
}
static int ble_op_set_ext_adv_enable(const void *data, uint16_t size) {
    check_unlocked();
    assert(size == sizeof(*ext_enable));
    ext_enable = data;
    return 0;
}
static void h2_ble_post(int type, const void *data, size_t size) {
    check_unlocked();
    (void)type;
    (void)data;
    assert(size == sizeof(h2_pal_ble_adv_set_event_t));
    ++posted;
}
static int h2_adv_set_stop(void *user, h2_pal_ble_adv_set_t *set);
typedef struct { int unused; } h2_ble_call_t;
int h2_ble_call_begin(h2_ble_call_t *call) { (void)call; check_unlocked(); return 0; }
void h2_ble_call_end(h2_ble_call_t *call) { (void)call; check_unlocked(); }
/* REAL_PROVIDER */
static void *start_thread(void *unused) {
    (void)unused;
    assert(h2_adv_set_start(NULL, &h2_ble.adv) == 0);
    return NULL;
}
static void *publish_thread(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 5000; ++i) {
        const uint8_t value = (uint8_t)(i % 2 + 1);
        const uint8_t bytes[] = {value, value, value, value};
        const h2_pal_ble_adv_data_t data = {.manufacturer_data = {bytes, sizeof(bytes)}};
        assert(h2_adv_set_data(NULL, &h2_ble.adv, &data) == 0);
    }
    return NULL;
}
static void *consume_thread(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 5000; ++i) {
        assert(h2_adv_set_start(NULL, &h2_ble.adv) == 0);
        assert(ext_data->data[5] == ext_data->data[6]);
        assert(ext_data->data[6] == ext_data->data[7]);
        assert(ext_data->data[7] == ext_data->data[8]);
        assert(sdk_hook != NULL);
        sdk_hook();
        assert(controller_fence != NULL);
        assert(controller_fence(controller_generation) == 0);
    }
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    (void)h2_connection_command_consumed;
    (void)h2_adv_apply;
    (void)h2_adv_set_destroy;
    (void)h2_legacy_stop_advertising;
    h2_ble.started = 1;
    const uint8_t identity[] = {1, 2, 3, 4};
    h2_pal_ble_adv_data_t data = {.manufacturer_data = {identity, sizeof(identity)}};
    h2_pal_ble_adv_params_t params = {
        .type = H2_PAL_BLE_ADV_TYPE_LEGACY, .mode = H2_PAL_BLE_ADV_MODE_CONNECTABLE,
        .interval_min_ms = 100, .interval_max_ms = 100,
        .primary_phy = H2_PAL_BLE_PHY_1M, .secondary_phy = H2_PAL_BLE_PHY_1M,
    };
    if (strcmp(argv[1], "early_hook") == 0 ||
        strcmp(argv[1], "early_restart") == 0 ||
        strcmp(argv[1], "stop_hook") == 0) {
        const int restart = strcmp(argv[1], "early_restart") == 0;
        stop_hook = strcmp(argv[1], "stop_hook") == 0;
        early_hook = !stop_hook;
        if (!restart && !stop_hook)
            params.type = H2_PAL_BLE_ADV_TYPE_EXTENDED;
        h2_pal_ble_adv_set_t *set = NULL;
        assert(h2_adv_set_create(NULL, &params, &set) == 0);
        assert(h2_adv_set_data(NULL, set, &data) == 0);
        assert(h2_adv_set_start(NULL, set) == 0);
        if (stop_hook)
            assert(h2_adv_set_stop(NULL, set) == 0);
        assert(registrations == 2 && sdk_hook != NULL);
        assert(h2_adv_commands.phase == 1u && controller_fence == NULL);
        if (restart) {
            set->started = 0;
            h2_restart_legacy_advertising();
            assert(h2_adv_commands.restart && sdk_starts == 1);
        }
        void (*hook)(void) = sdk_hook;
        sdk_hook = NULL;
        hook();
        assert(controller_fence != NULL && h2_adv_commands.phase == 2u);
        assert(h2_adv_set_start(NULL, set) == H2_PAL_ERR_WOULD_BLOCK);
        assert(controller_fence(controller_generation) == 0);
        assert(h2_adv_commands.phase == 0u);
        if (restart) {
            assert(sdk_hook != NULL);
            hook = sdk_hook;
            sdk_hook = NULL;
            hook();
            assert(sdk_starts == 2);
        } else {
            assert(h2_adv_set_start(NULL, set) == 0);
        }
        return 0;
    }
    if (strcmp(argv[1], "extended") == 0 || strcmp(argv[1], "submitting") == 0 ||
        strcmp(argv[1], "state_race") == 0 || strcmp(argv[1], "registration_error") == 0 ||
        strcmp(argv[1], "fence_error") == 0 || strcmp(argv[1], "pending_init") == 0) {
        params.type = H2_PAL_BLE_ADV_TYPE_EXTENDED;
        h2_pal_ble_adv_set_t *set = NULL;
        assert(h2_adv_set_create(NULL, &params, &set) == 0);
        assert(h2_adv_set_data(NULL, set, &data) == 0);
        if (strcmp(argv[1], "registration_error") == 0) {
            registration_error = H2_PAL_ERR_IO;
            assert(h2_adv_set_start(NULL, set) == H2_PAL_ERR_IO);
            assert(sdk_starts == 0);
            registration_error = 0;
        }
        if (strcmp(argv[1], "pending_init") == 0) {
            h2_ble.started = 0;
            h2_ble.starting = 1;
            assert(h2_adv_set_start(NULL, set) == 0);
            assert(posted == 0 && sdk_starts == 0);
            return 0;
        }
        if (strcmp(argv[1], "state_race") == 0) {
            const uint8_t uniform[] = {1, 1, 1, 1};
            data.manufacturer_data.data = uniform;
            assert(h2_adv_set_data(NULL, set, &data) == 0);
            pthread_t publisher, consumer;
            assert(pthread_create(&publisher, NULL, publish_thread, NULL) == 0);
            assert(pthread_create(&consumer, NULL, consume_thread, NULL) == 0);
            assert(pthread_join(publisher, NULL) == 0);
            assert(pthread_join(consumer, NULL) == 0);
            return 0;
        }
        if (strcmp(argv[1], "submitting") == 0) {
            pthread_t thread;
            atomic_store(&hold_submit, 1);
            assert(pthread_create(&thread, NULL, start_thread, NULL) == 0);
            while (!atomic_load(&submit_entered))
                sched_yield();
            assert(h2_adv_set_start(NULL, set) == H2_PAL_ERR_WOULD_BLOCK);
            assert(h2_adv_set_stop(NULL, set) == H2_PAL_ERR_WOULD_BLOCK);
            atomic_store(&release_submit, 1);
            assert(pthread_join(thread, NULL) == 0);
        } else {
            assert(h2_adv_set_start(NULL, set) == 0);
        }
        if (strcmp(argv[1], "fence_error") == 0) {
            fence_error = H2_PAL_ERR_IO;
            assert(sdk_hook != NULL);
            sdk_hook();
            assert(h2_adv_set_start(NULL, set) == H2_PAL_ERR_WOULD_BLOCK);
            assert(h2_adv_set_stop(NULL, set) == 0);
            assert(h2_adv_set_start(NULL, set) == H2_PAL_ERR_WOULD_BLOCK);
            return 0;
        }
        /* Btstack forwards the same pointers into the controller queue. */
        assert(sdk_hook != NULL);
        sdk_hook();
        assert(controller_fence != NULL);
        assert(h2_adv_set_start(NULL, set) == H2_PAL_ERR_WOULD_BLOCK);
        /* Both native queue stages consume these pointers after return. */
        assert(ext_params->channel_map == 7 && ext_params->primary_phy == 1);
        assert(ext_data->length == 9 && ext_data->data[5] == 1);
        assert(ext_enable->enable == 1 && ext_enable->number_of_sets == 1);
        assert(controller_fence(controller_generation) == 0);
        assert(h2_adv_set_start(NULL, set) == 0);
    } else {
        assert(h2_legacy_set_adv_data(NULL, &data) == 0);
        if (strcmp(argv[1], "stop_error") == 0) {
            assert(h2_legacy_start_advertising(NULL, &params) == 0);
            stop_error = H2_PAL_ERR_IO;
            assert(h2_legacy_stop_advertising(NULL) == H2_PAL_ERR_IO);
            assert(h2_ble.adv.start_requested && h2_ble.adv.started);
            stop_error = 0;
            assert(h2_legacy_stop_advertising(NULL) == 0);
            assert(!h2_ble.adv.start_requested && !h2_ble.adv.started);
        } else if (strcmp(argv[1], "deferred") == 0) {
            assert(h2_legacy_start_advertising(NULL, &params) == 0);
            h2_gatt_lock();
            h2_ble.adv.started = 0;
            h2_gatt_unlock();
            h2_restart_legacy_advertising();
            assert(sdk_starts == 1);
            assert(sdk_hook != NULL);
            sdk_hook();
            assert(sdk_starts == 1 && controller_fence != NULL);
            assert(controller_fence(controller_generation) == 0);
            sdk_hook();
            assert(sdk_starts == 2);
        } else if (strcmp(argv[1], "legacy_borrow") == 0) {
            assert(h2_legacy_start_advertising(NULL, &params) == 0);
            const uint8_t changed[] = {5, 6, 7, 8};
            data.manufacturer_data.data = changed;
            assert(h2_legacy_set_adv_data(NULL, &data) == 0);
            assert(legacy_data[5] == 1);
        } else if (strcmp(argv[1], "failed_update") == 0) {
            const struct h2_pal_ble_adv_set previous = h2_ble.adv;
            const uint8_t changed[] = {5, 6, 7, 8};
            data.manufacturer_data.data = changed;
            data.local_name = "This name cannot fit in a legacy scan response";
            assert(h2_legacy_set_adv_data(NULL, &data) == H2_PAL_ERR_NO_SPACE);
            assert(memcmp(&previous, &h2_ble.adv, sizeof(previous)) == 0);
        } else {
            assert(strcmp(argv[1], "null_uuids") == 0);
            data.service_uuid_count = 1;
            data.service_uuids = NULL;
            assert(h2_legacy_set_adv_data(NULL, &data) == H2_PAL_ERR_INVALID_ARG);
        }
    }
    return 0;
}
