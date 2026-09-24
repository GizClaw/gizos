#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include "h2_atomic.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/os/h2_pal_system_event.h"
typedef uint8_t u8;
typedef uint16_t hci_con_handle_t;
struct h2_pal_ble_adv_set { int used, started, start_requested; };
static struct {
    int starting, started, stopping, stop_worker, native_created, start_failed;
    uint16_t conn_handle, retiring_connection;
    unsigned command_rearm_needed, command_rearm_active;
    unsigned conn_pending, conn_submitting, conn_hook_skipped;
    uint32_t conn_generation;
    struct h2_pal_ble_adv_set adv;
} h2_ble;
static struct { unsigned phase; } h2_adv_commands;
int ble_op_regist_thread_call(void (*hook)(void)) { (void)hook; return 0; }
const uint64_t config_btctler_le_features = 0;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
void h2_gatt_lock(void) { assert(pthread_mutex_lock(&gate) == 0); }
void h2_gatt_unlock(void) { assert(pthread_mutex_unlock(&gate) == 0); }
static h2_atomic_int_t waiting, stopped, started, exited, unregistered, stop_returned, borrow_active, disconnected;
enum { ERROR_CODE_CONNECTION_TERMINATED_BY_LOCAL_HOST = 0x16 };
static int adv_result, disconnect_result, exit_result, disconnect_calls, init_result;
static h2_atomic_int_t release_borrow, hold_role, role_entered, release_role;
static int callback_mode, self_stop_mode;
static int h2_ble_stop(void *user);
static _Thread_local int identity;
static _Thread_local const char *task_name;
const void *h2_jieli_sdk_task_current(void) { return &identity; }
const char *os_current_task(void) { return task_name; }
void os_time_dly(int ticks) {
    assert(ticks == 1);
    h2_atomic_store(&waiting, 1);
    sched_yield();
}
void h2_ble_log(const char *format, ...) { (void)format; }
void lmp_set_sniff_disable(void) {}
const uint8_t *h2_ble_base_mac(void) {
    static const uint8_t address[6] = {0};
    return address;
}
void lib_make_ble_address(uint8_t *out, uint8_t *in) { memcpy(out, in, 6); }
int le_controller_set_mac(void *address) { (void)address; return 0; }
int btstack_init(void) { return init_result; }
u8 get_ble_gatt_role(void) {
    if (h2_atomic_load(&hold_role)) {
        h2_atomic_store(&role_entered, 1);
        while (!h2_atomic_load(&release_role))
            sched_yield();
    }
    return 0;
}
void ble_stack_gatt_role(int role) { (void)role; }
static void sdk_reenter_gate(void) {
    h2_gatt_lock();
    h2_gatt_unlock();
}
int h2_adv_set_stop(void *user, h2_pal_ble_adv_set_t *set) {
    (void)user;
    (void)set;
    sdk_reenter_gate();
    return adv_result;
}
int h2_adv_apply(struct h2_pal_ble_adv_set *set) { (void)set; return 0; }
int h2_ble_cmd_result(int result) { return result; }
int ble_op_disconnect(uint16_t connection) {
    assert(connection == 42);
    ++disconnect_calls;
    sdk_reenter_gate();
    return disconnect_result;
}
int btstack_exit(void) {
    sdk_reenter_gate();
    assert(!h2_atomic_load(&borrow_active));
    h2_atomic_fetch_add(&exited, 1);
    return exit_result;
}
int h2_unregister_gatt(void *user) {
    (void)user;
    h2_atomic_fetch_add(&unregistered, 1);
    return 0;
}
enum { BLE_CMD_STACK_EXIT = 14 };
int ble_user_cmd_prepare(int cmd, int argc, ...) {
    assert(cmd == BLE_CMD_STACK_EXIT);
    (void)argc;
    return exit_result;
}
void h2_ble_post(int type, const void *data, size_t length) {
    (void)data;
    (void)length;
    sdk_reenter_gate();
    if (type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED) {
        const h2_pal_ble_disconnected_info_t *info = data;
        assert(length == sizeof(*info) && info->conn_handle == 42 && info->reason == 0x16);
        assert(h2_atomic_load(&exited) != 0);
        h2_atomic_fetch_add(&disconnected, 1);
    }
    if (type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STOPPED)
        h2_atomic_fetch_add(&stopped, 1);
    if (type == H2_PAL_SYSTEM_EVENT_TYPE_BLE_HOST_STARTED)
        h2_atomic_fetch_add(&started, 1);
}
int h2_notify(void *user, uint16_t connection, uint16_t attribute, const uint8_t *data, size_t size) {
    (void)user;
    (void)connection;
    (void)attribute;
    (void)data;
    (void)size;
    if (self_stop_mode) {
        assert(h2_ble_stop(NULL) == H2_PAL_ERR_BUSY);
        return 0;
    }
    h2_atomic_store(&borrow_active, 1);
    while (!h2_atomic_load(&release_borrow))
        sched_yield();
    assert(!h2_atomic_load(&exited));
    h2_atomic_store(&borrow_active, 0);
    return 0;
}
int h2_att_write(uint16_t connection, uint16_t handle, uint16_t transaction,
                 uint16_t offset, uint8_t *data, uint16_t size) {
    (void)transaction;
    (void)offset;
    return h2_notify(NULL, connection, handle, data, size);
}
void h2_adv_command_consumed(void) { (void)h2_notify(NULL, 0, 0, NULL, 0); }
int ble_cmd_handler_is_idle(void) { sdk_reenter_gate(); return 1; }
/* REAL_PROVIDER */
static void *borrow_thread(void *unused) {
    (void)unused;
    if (callback_mode == 2)
        h2_connection_command_consumed();
    else if (callback_mode)
        assert(h2_att_write_retained(1, 9, 0, 0, NULL, 0) == 0);
    else
        assert(h2_notify_retained(NULL, 1, 6, NULL, 0) == 0);
    return NULL;
}
static void *init_thread(void *unused) {
    (void)unused;
    bt_ble_init();
    return NULL;
}
static void *stop_thread(void *unused) {
    (void)unused;
    assert(h2_ble_stop(NULL) == H2_PAL_OK);
    h2_atomic_store(&stop_returned, 1);
    return NULL;
}
static void start_stop_thread(pthread_t *thread) {
    assert(pthread_create(thread, NULL, stop_thread, NULL) == 0);
    while (!h2_atomic_load(&waiting) && !h2_atomic_load(&stop_returned))
        sched_yield();
    assert(!h2_atomic_load(&stop_returned));
    assert(!h2_atomic_load(&exited));
}
static void h2_fixture_atomic_cleanup(void) {
    h2_atomic_destroy(&waiting);
    h2_atomic_destroy(&stopped);
    h2_atomic_destroy(&started);
    h2_atomic_destroy(&exited);
    h2_atomic_destroy(&unregistered);
    h2_atomic_destroy(&stop_returned);
    h2_atomic_destroy(&borrow_active);
    h2_atomic_destroy(&disconnected);
    h2_atomic_destroy(&release_borrow);
    h2_atomic_destroy(&hold_role);
    h2_atomic_destroy(&role_entered);
    h2_atomic_destroy(&release_role);
}
int main(int argc, char **argv) {
    assert(atexit(h2_fixture_atomic_cleanup) == 0);
    assert(h2_atomic_init(&waiting, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&stopped, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&started, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&exited, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&unregistered, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&stop_returned, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&borrow_active, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&disconnected, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&release_borrow, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&hold_role, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&role_entered, 0) == H2_ATOMIC_OK);
    assert(h2_atomic_init(&release_role, 0) == H2_ATOMIC_OK);

    assert(argc == 2);
    h2_ble.started = 1;
    h2_ble.adv.used = 1;
    if (strcmp(argv[1], "init_error") == 0) {
        h2_ble.started = 0;
        h2_ble.adv.used = 0;
        init_result = -1;
        assert(h2_ble_start_retained(NULL) == H2_PAL_ERR_IO);
        assert(h2_ble_stop(NULL) == H2_PAL_ERR_IO);
        assert(!h2_atomic_load(&exited) && !h2_atomic_load(&unregistered));
        assert(h2_ble_start_retained(NULL) == H2_PAL_ERR_BUSY);
        return 0;
    }
    if (strcmp(argv[1], "adv_error") == 0 || strcmp(argv[1], "disconnect_error") == 0 || strcmp(argv[1], "exit_error") == 0) {
        if (strcmp(argv[1], "adv_error") == 0)
            adv_result = H2_PAL_ERR_IO;
        if (strcmp(argv[1], "disconnect_error") == 0) {
            h2_ble.conn_handle = 42;
            disconnect_result = H2_PAL_ERR_IO;
        }
        if (strcmp(argv[1], "exit_error") == 0) {
            h2_ble.conn_handle = 42;
            exit_result = -1;
        }
        assert(h2_ble_stop(NULL) == H2_PAL_ERR_IO);
        assert(h2_ble.started && h2_ble.adv.used && !h2_atomic_load(&stopped));
        assert(!h2_atomic_load(&unregistered));
        assert(h2_ble_start_retained(NULL) == H2_PAL_ERR_BUSY);
        adv_result = disconnect_result = exit_result = 0;
        assert(h2_ble_stop(NULL) == H2_PAL_OK);
        assert(!h2_ble.started && h2_atomic_load(&stopped) == 1 && h2_atomic_load(&unregistered) == 1);
        if (strcmp(argv[1], "exit_error") == 0) {
            assert(disconnect_calls == 1);
            assert(h2_atomic_load(&disconnected) == 1);
        }
        return 0;
    }
    if (strcmp(argv[1], "disconnect_event") == 0) {
        h2_ble.conn_handle = 42;
        assert(h2_ble_stop(NULL) == 0);
        assert(h2_atomic_load(&disconnected) == 1 && h2_atomic_load(&stopped) == 1);
        assert(h2_ble_stop(NULL) == 0);
        assert(h2_atomic_load(&disconnected) == 1 && h2_atomic_load(&stopped) == 1);
        return 0;
    }
    if (strcmp(argv[1], "retained") == 0 || strcmp(argv[1], "retained_callback") == 0 || strcmp(argv[1], "retained_command") == 0 || strcmp(argv[1], "self_stop") == 0) {
        callback_mode = strcmp(argv[1], "retained_command") == 0 ? 2 : strcmp(argv[1], "retained") != 0;
        if (strcmp(argv[1], "self_stop") == 0) {
            self_stop_mode = 1;
            borrow_thread(NULL);
            assert(h2_ble_stop(NULL) == 0);
            return 0;
        }
        pthread_t borrower, thread;
        assert(pthread_create(&borrower, NULL, borrow_thread, NULL) == 0);
        while (!h2_atomic_load(&borrow_active))
            sched_yield();
        start_stop_thread(&thread);
        assert(h2_ble_start_retained(NULL) == H2_PAL_ERR_BUSY);
        h2_atomic_store(&release_borrow, 1);
        assert(pthread_join(borrower, NULL) == 0);
        assert(pthread_join(thread, NULL) == 0);
        assert(h2_atomic_load(&exited) == 1 && h2_atomic_load(&stopped) == 1);
        return 0;
    }
    if (strcmp(argv[1], "admitted_start") == 0) {
        h2_ble.started = 0;
        h2_ble.adv.used = 0;
        h2_ble_call_t call;
        assert(h2_ble_call_begin(&call) == 0);
        pthread_t thread;
        start_stop_thread(&thread);
        assert(h2_ble_start(NULL) == 0);
        bt_ble_init();
        h2_ble_call_end(&call);
        assert(pthread_join(thread, NULL) == 0);
        assert(h2_atomic_load(&exited) == 1 && h2_atomic_load(&stopped) == 1);
        assert(!h2_atomic_load(&started));
        return 0;
    }
    if (strcmp(argv[1], "init_publication") == 0) {
        h2_ble.started = 0;
        h2_ble.starting = 1;
        h2_atomic_store(&hold_role, 1);
        pthread_t initializer, stopper;
        assert(pthread_create(&initializer, NULL, init_thread, NULL) == 0);
        while (!h2_atomic_load(&role_entered))
            sched_yield();
        start_stop_thread(&stopper);
        h2_atomic_store(&release_role, 1);
        assert(pthread_join(initializer, NULL) == 0);
        assert(pthread_join(stopper, NULL) == 0);
        assert(!h2_atomic_load(&started) && h2_atomic_load(&stopped) == 1);
        return 0;
    }
    if (strcmp(argv[1], "pending_init") == 0) {
        h2_ble.started = 0;
        h2_ble.starting = 1;
        pthread_t thread;
        start_stop_thread(&thread);
        bt_ble_init();
        assert(pthread_join(thread, NULL) == 0);
        assert(!h2_atomic_load(&started) && h2_atomic_load(&stopped) == 1);
        return 0;
    }
    if (strcmp(argv[1], "init_dispatcher") == 0) {
        h2_ble.started = 0;
        h2_ble.starting = 1;
        task_name = "app_core";
        assert(h2_ble_stop(NULL) == H2_PAL_ERR_BUSY);
        assert(!h2_ble.stopping && h2_ble.starting);
        bt_ble_init();
        assert(h2_atomic_load(&started) == 1);
        assert(h2_ble_stop(NULL) == 0);
        return 0;
    }
    assert(strcmp(argv[1], "late_init") == 0);
    assert(h2_ble_stop(NULL) == 0);
    bt_ble_init();
    assert(!h2_ble.started && !h2_atomic_load(&started) && h2_atomic_load(&stopped) == 1);
    return 0;
}
