#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include "h2_atomic.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2_jieli_wl82_atomic.h"
typedef uint16_t hci_con_handle_t;
enum {
    H2_JIELI_GATT_SERVICE_HANDLE = 4,
    H2_JIELI_GATT_TX_VALUE_HANDLE = 6,
    H2_JIELI_GATT_TX_CCCD_HANDLE = 7,
    H2_JIELI_GATT_RX_VALUE_HANDLE = 9,
};
static const uint8_t h2_service_uuid[16] = {1};
static const uint8_t h2_tx_uuid[16] = {2};
static const uint8_t h2_rx_uuid[16] = {3};
static struct {
    h2_pal_ble_gatt_characteristic_t characteristics[2];
    int gatt_registered;
} h2_ble;
static int h2_uuid_equal(const h2_pal_ble_uuid_t *uuid, const uint8_t *value) {
    return uuid->len == 16 && uuid->data != NULL && memcmp(uuid->data, value, 16) == 0;
}
static void h2_att_trace_record(int kind, uint16_t handle, uint16_t offset,
                                const uint8_t *buffer, uint16_t size) {
    (void)kind;
    (void)handle;
    (void)offset;
    (void)buffer;
    (void)size;
}
static void att_set_ccc_config(uint16_t handle, uint8_t value) {
    (void)handle;
    (void)value;
}
static void h2_ble_post(int type, const void *data, size_t size) {
    (void)type;
    (void)data;
    (void)size;
}
static h2_atomic_int_t callback_entered, release_callback, unregister_returned;
static h2_atomic_int_t unregister_waiting, context_freed;
static _Thread_local int task_identity;
const void *h2_jieli_sdk_task_current(void) { return &task_identity; }
void os_time_dly(int ticks) {
    assert(ticks == 1);
    h2_atomic_store(&unregister_waiting, 1);
    sched_yield();
}
/* REAL_PROVIDER */
static int self_unregister;
static h2_pal_result_t write_callback(void *user, const h2_pal_ble_gatt_access_t *access,
                                     const uint8_t *data, size_t len) {
    assert(user == &context_freed && access->attr_handle == 9 && data[0] == 42 && len == 1);
    if (self_unregister) {
        assert(h2_unregister_gatt(NULL) == H2_PAL_ERR_BUSY);
        return H2_PAL_OK;
    }
    h2_atomic_store(&callback_entered, 1);
    while (!h2_atomic_load(&release_callback))
        sched_yield();
    assert(!h2_atomic_load(&context_freed));
    return H2_PAL_OK;
}
static void *write_thread(void *unused) {
    (void)unused;
    uint8_t data = 42;
    assert(h2_att_write(1, 9, 0, 0, &data, 1) == 0);
    return NULL;
}
static void *unregister_thread(void *unused) {
    (void)unused;
    assert(h2_unregister_gatt(NULL) == H2_PAL_OK);
    h2_atomic_store(&context_freed, 1);
    h2_atomic_store(&unregister_returned, 1);
    return NULL;
}
static void h2_fixture_atomic_cleanup(void) {
    h2_atomic_destroy(&callback_entered);
    h2_atomic_destroy(&release_callback);
    h2_atomic_destroy(&unregister_returned);
    h2_atomic_destroy(&unregister_waiting);
    h2_atomic_destroy(&context_freed);
}
int main(int argc, char **argv) {
    assert(atexit(h2_fixture_atomic_cleanup) == 0);
    assert(h2_atomic_init(&callback_entered, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&release_callback, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&unregister_returned, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&unregister_waiting, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&context_freed, 0) == H2_ATOMIC_OK);

    assert(argc == 2);
    h2_pal_ble_gatt_characteristic_t characteristics[2] = {
        {.uuid = {.data = h2_tx_uuid, .len = 16}},
        {.uuid = {.data = h2_rx_uuid, .len = 16}, .write = write_callback, .user = &context_freed},
    };
    const h2_pal_ble_gatt_service_t service = {
        .uuid = {.data = h2_service_uuid, .len = 16}, .primary = 1,
        .characteristics = characteristics, .characteristic_count = 2,
    };
    assert(h2_register_gatt(NULL, &service, 1) == H2_PAL_OK);
    if (strcmp(argv[1], "self_unregister") == 0) {
        self_unregister = 1;
        write_thread(NULL);
        assert(h2_unregister_gatt(NULL) == H2_PAL_OK);
        return 0;
    }
    pthread_t writer, unregisterer;
    assert(pthread_create(&writer, NULL, write_thread, NULL) == 0);
    while (!h2_atomic_load(&callback_entered))
        sched_yield();
    assert(pthread_create(&unregisterer, NULL, unregister_thread, NULL) == 0);
    while (!h2_atomic_load(&unregister_waiting) && !h2_atomic_load(&unregister_returned))
        sched_yield();
    assert(!h2_atomic_load(&unregister_returned));
    assert(h2_register_gatt(NULL, &service, 1) == H2_PAL_ERR_BUSY);
    h2_atomic_store(&release_callback, 1);
    assert(pthread_join(writer, NULL) == 0);
    assert(pthread_join(unregisterer, NULL) == 0);
    assert(h2_atomic_load(&context_freed));
    uint8_t data = 42;
    assert(h2_att_write(1, 9, 0, 0, &data, 1) == 0);
    assert(h2_register_gatt(NULL, &service, 1) == H2_PAL_OK);
    assert(h2_unregister_gatt(NULL) == H2_PAL_OK);
    return 0;
}
