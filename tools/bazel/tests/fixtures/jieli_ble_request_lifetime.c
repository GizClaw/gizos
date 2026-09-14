#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_ble.h"
struct conn_update_param_t { uint16_t interval_min, interval_max, latency, timeout; };
static struct {
    uint16_t conn_handle;
    struct conn_update_param_t conn_params;
    unsigned conn_pending, conn_submitting;
    uint32_t conn_generation;
} h2_ble;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static void h2_gatt_lock(void) { assert(pthread_mutex_lock(&gate) == 0); }
static void h2_gatt_unlock(void) { assert(pthread_mutex_unlock(&gate) == 0); }
static void check_unlocked(void) { h2_gatt_lock(); h2_gatt_unlock(); }
#define h2_ble_log(...) check_unlocked()
static int h2_ble_cmd_result(int value) { return value; }
static void (*sdk_hook)(void);
static const struct conn_update_param_t *borrowed;
static atomic_int blocked, entered, release_call;
static int registration_error, request_error, idle = 1, requests;
int ble_op_regist_thread_call(void (*hook)(void)) {
    check_unlocked();
    if (registration_error)
        return registration_error;
    sdk_hook = hook;
    return 0;
}
int ble_cmd_handler_is_idle(void) { check_unlocked(); return idle; }
void stack_run_loop_resume(void) { check_unlocked(); }
static int ble_op_conn_param_request(uint16_t handle, const struct conn_update_param_t *request) {
    check_unlocked();
    assert(handle == 42);
    ++requests;
    borrowed = request;
    if (atomic_load(&blocked) && requests == 1) {
        atomic_store(&entered, 1);
        while (!atomic_load(&release_call))
            sched_yield();
    }
    return request_error;
}
/* REAL_PROVIDER */
static const h2_pal_ble_connection_params_t first = {
    .interval_min_ms = 50, .interval_max_ms = 100, .latency = 2, .supervision_timeout_ms = 2000,
};
static const h2_pal_ble_connection_params_t second = {
    .interval_min_ms = 100, .interval_max_ms = 150, .latency = 3, .supervision_timeout_ms = 3000,
};
static void consume(void) {
    assert(borrowed != NULL);
    assert(borrowed->interval_min == 40 && borrowed->interval_max == 80);
    assert(borrowed->latency == 2 && borrowed->timeout == 200);
    assert(sdk_hook != NULL);
    sdk_hook();
}
static void *submit(void *unused) {
    (void)unused;
    assert(h2_update_connection(NULL, 42, &first) == 0);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    h2_ble.conn_handle = 42;
    if (strcmp(argv[1], "registration_error") == 0) {
        registration_error = H2_PAL_ERR_IO;
        assert(h2_update_connection(NULL, 42, &first) == H2_PAL_ERR_IO);
        assert(requests == 0);
        registration_error = 0;
    } else if (strcmp(argv[1], "request_error") == 0) {
        request_error = H2_PAL_ERR_IO;
        assert(h2_update_connection(NULL, 42, &first) == H2_PAL_ERR_IO);
        request_error = 0;
    }
    if (strcmp(argv[1], "submitting") == 0) {
        pthread_t thread;
        atomic_store(&blocked, 1);
        assert(pthread_create(&thread, NULL, submit, NULL) == 0);
        while (!atomic_load(&entered))
            sched_yield();
        if (sdk_hook != NULL)
            sdk_hook();
        assert(h2_update_connection(NULL, 42, &second) == H2_PAL_ERR_WOULD_BLOCK);
        atomic_store(&release_call, 1);
        assert(pthread_join(thread, NULL) == 0);
        atomic_store(&blocked, 0);
    } else {
        assert(h2_update_connection(NULL, 42, &first) == 0);
        if (strcmp(argv[1], "consumer_busy") == 0 && sdk_hook != NULL) {
            idle = 0;
            sdk_hook();
            idle = 1;
        }
        assert(h2_update_connection(NULL, 42, &second) == H2_PAL_ERR_WOULD_BLOCK);
    }
    consume();
    assert(h2_update_connection(NULL, 42, &second) == 0);
    assert(borrowed->interval_min == 80 && borrowed->latency == 3);
    return 0;
}
