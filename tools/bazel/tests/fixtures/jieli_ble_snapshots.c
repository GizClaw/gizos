#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/os/h2_pal_system_event.h"
enum { HCI_EVENT_DISCONNECTION_COMPLETE = 5, ATT_EVENT_MTU_EXCHANGE_COMPLETE = 0xb5, H2_JIELI_ATT_MTU = 512 };
static struct {
    uint16_t conn_handle, mtu;
    struct { int started; } adv;
} h2_ble;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
void h2_gatt_lock(void) { assert(pthread_mutex_lock(&gate) == 0); }
void h2_gatt_unlock(void) { assert(pthread_mutex_unlock(&gate) == 0); }
static void sdk_reenter_gate(void) {
    h2_gatt_lock();
    h2_gatt_unlock();
}
void h2_ble_log(const char *format, ...) {
    (void)format;
    sdk_reenter_gate();
}
static int mtu_calls, disconnect_calls, posts;
static uint16_t sent_mtu;
uint16_t att_event_mtu_exchange_complete_get_handle(const uint8_t *packet) {
    return (uint16_t)(packet[2] | (packet[3] << 8));
}
uint16_t att_event_mtu_exchange_complete_get_MTU(const uint8_t *packet) {
    return (uint16_t)(packet[4] | (packet[5] << 8));
}
uint16_t hci_event_disconnection_complete_get_connection_handle(const uint8_t *packet) {
    return (uint16_t)(packet[3] | (packet[4] << 8));
}
int ble_op_att_set_send_mtu(uint16_t mtu) {
    sdk_reenter_gate();
    ++mtu_calls;
    sent_mtu = mtu;
    return 0;
}
int ble_op_att_send_init(uint16_t handle, void *buffer, uint16_t size, uint16_t mtu) {
    sdk_reenter_gate();
    assert(handle == 0 && buffer == NULL && size == 0 && mtu == 0);
    ++disconnect_calls;
    return 0;
}
void h2_restart_legacy_advertising(void) {}
void h2_ble_post(int type, const void *data, size_t size) {
    sdk_reenter_gate();
    assert(type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED || type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_MTU_CHANGED);
    assert(data != NULL && size != 0);
    ++posts;
}
/* REAL_PROVIDER */
static void mtu_event(uint16_t handle, uint16_t mtu) {
    uint8_t packet[6] = {0, 0, (uint8_t)handle, (uint8_t)(handle >> 8), (uint8_t)mtu, (uint8_t)(mtu >> 8)};
    event(ATT_EVENT_MTU_EXCHANGE_COMPLETE, packet, sizeof(packet));
}
static void disconnect_event(uint16_t handle) {
    uint8_t packet[6] = {5, 4, 0, (uint8_t)handle, (uint8_t)(handle >> 8), 0x13};
    event(HCI_EVENT_DISCONNECTION_COMPLETE, packet, 5);
}
static atomic_int go;
static void *connections(void *unused) {
    (void)unused;
    while (!atomic_load(&go))
        sched_yield();
    for (unsigned i = 0; i < 10000; ++i) {
        connected(42);
        disconnect_event(42);
    }
    return NULL;
}
static void *snapshots(void *unused) {
    (void)unused;
    while (!atomic_load(&go))
        sched_yield();
    for (unsigned i = 0; i < 10000; ++i) {
        uint16_t mtu = 0xffff;
        const int result = h2_exchange_mtu(NULL, 42, &mtu, 0);
        assert(result == H2_PAL_ERR_INVALID_ARG || (result == 0 && mtu == 23));
    }
    return NULL;
}
static void *trace_writer(void *unused) {
    (void)unused;
    while (!atomic_load(&go))
        sched_yield();
    for (unsigned i = 0; i < 10000; ++i)
        h2_att_trace_record(0, (uint16_t)i, (uint16_t)i, NULL, (uint16_t)i);
    return NULL;
}
static void *trace_reader(void *unused) {
    (void)unused;
    while (!atomic_load(&go))
        sched_yield();
    for (unsigned i = 0; i < 10000; ++i)
        h2_att_trace_dump();
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    connected(42);
    if (strcmp(argv[1], "failed_disconnect") == 0) {
        uint8_t packet[6] = {5, 4, 1, 42, 0, 0x13};
        event(HCI_EVENT_DISCONNECTION_COMPLETE, packet, 5);
        assert(h2_ble.conn_handle == 42 && h2_ble.mtu == 23);
        assert(disconnect_calls == 0 && posts == 0);
    } else if (strcmp(argv[1], "short_events") == 0) {
        uint8_t packet[6] = {0, 0, 42, 0, 200, 0};
        event(ATT_EVENT_MTU_EXCHANGE_COMPLETE, packet, 1);
        packet[2] = 0;
        packet[3] = 42;
        packet[4] = 0;
        event(HCI_EVENT_DISCONNECTION_COMPLETE, packet, 1);
        assert(h2_ble.conn_handle == 42 && h2_ble.mtu == 23);
        assert(mtu_calls == 0 && disconnect_calls == 0 && posts == 0);
    } else if (strcmp(argv[1], "stale_mtu") == 0) {
        mtu_event(41, 200);
        assert(h2_ble.mtu == 23 && mtu_calls == 0 && posts == 0);
    } else if (strcmp(argv[1], "invalid_mtu") == 0) {
        const uint16_t invalid[] = {0, 2, 22, 513, 65535};
        for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            mtu_event(42, invalid[i]);
            assert(h2_ble.mtu == 23 && mtu_calls == 0 && posts == 0);
        }
    } else if (strcmp(argv[1], "stale_disconnect") == 0) {
        disconnect_event(41);
        assert(h2_ble.conn_handle == 42 && h2_ble.mtu == 23 && disconnect_calls == 0 && posts == 0);
    } else if (strcmp(argv[1], "current_events") == 0) {
        mtu_event(42, 200);
        assert(h2_ble.mtu == 200 && mtu_calls == 1 && sent_mtu == 197 && posts == 1);
        disconnect_event(42);
        assert(h2_ble.conn_handle == 0 && h2_ble.mtu == 0 && disconnect_calls == 1 && posts == 2);
    } else {
        pthread_t writer, reader;
        const int trace = strcmp(argv[1], "trace") == 0;
        assert(trace || strcmp(argv[1], "snapshots") == 0);
        assert(pthread_create(&writer, NULL, trace ? trace_writer : connections, NULL) == 0);
        assert(pthread_create(&reader, NULL, trace ? trace_reader : snapshots, NULL) == 0);
        atomic_store(&go, 1);
        assert(pthread_join(writer, NULL) == 0);
        assert(pthread_join(reader, NULL) == 0);
    }
    return 0;
}
