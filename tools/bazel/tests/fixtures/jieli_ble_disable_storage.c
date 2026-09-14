#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/os/h2_pal_system_event.h"
struct h2_pal_ble_adv_set {
    h2_pal_ble_adv_params_t params;
    int used, started, start_requested;
};
static struct {
    struct h2_pal_ble_adv_set adv;
    unsigned command_rearm_needed, command_rearm_active;
    int stopping;
} h2_ble;
static struct { unsigned submitting, restart, phase, hook_skipped; } h2_adv_commands;
static int locked;
static void h2_gatt_lock(void) { assert(!locked); locked = 1; }
static void h2_gatt_unlock(void) { assert(locked); locked = 0; }
void stack_run_loop_resume(void) { assert(!locked); }
static const void *queued;
static uint16_t queued_size;
static int h2_ble_cmd_result(int result) { return result; }
static int ble_op_set_ext_adv_enable(const void *data, uint16_t size) {
    queued = data;
    queued_size = size;
    return 0;
}
static int ble_op_adv_enable(int enabled) { (void)enabled; return 0; }
static void h2_ble_post(int type, const void *data, size_t size) {
    (void)type;
    (void)data;
    assert(size == sizeof(h2_pal_ble_adv_set_event_t));
}
static void h2_connection_command_consumed(void) {}
static int ble_op_regist_thread_call(void (*hook)(void)) {
    assert(!locked);
    (void)hook;
    return 0;
}
#define h2_ble_log(...) assert(!locked)
/* REAL_PROVIDER */
int main(void) {
    h2_ble.adv.used = h2_ble.adv.started = h2_ble.adv.start_requested = 1;
    h2_ble.adv.params.type = H2_PAL_BLE_ADV_TYPE_EXTENDED;
    assert(h2_adv_set_stop(NULL, &h2_ble.adv) == 0);
    assert(queued != NULL && queued_size == sizeof(struct h2_ext_adv_enable));
    /* Match the SDK: consume the borrowed descriptor after the PAL call
     * returns. ASan makes a stack-lifetime violation deterministic. */
    const struct h2_ext_adv_enable *disable = queued;
    assert(disable->enable == 0 && disable->number_of_sets == 1);
    assert(disable->handle == 0 && disable->duration == 0 && disable->max_events == 0);
    return 0;
}
