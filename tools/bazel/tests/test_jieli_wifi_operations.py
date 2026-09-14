"""Radio operations cannot invalidate an active or abandoned scan."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class WifiOperationsTest(unittest.TestCase):
    def test_reentry_and_failure_release(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_wifi.c").read_text()
        begin = source.index("static unsigned wifi_operation_busy;")
        guard = source[begin:source.index("const h2_pal_wifi_sta_api_t *h2_jieli", begin)]
        state = source[source.index("enum { SCAN_IDLE"):
                       source.index("static void post_system_event")]
        fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include "h2/pal/hal/h2_pal_wifi.h"
static pthread_mutex_t state_gate = PTHREAD_MUTEX_INITIALIZER;
static unsigned wifi_callbacks_active;
static void wifi_state_lock(void) { assert(pthread_mutex_lock(&state_gate) == 0); }
static void wifi_state_unlock(void) { assert(pthread_mutex_unlock(&state_gate) == 0); }
static int clears;
static void wifi_clear_scan_result(void) { ++clears; }
''' + state + r'''
static int calls;
static unsigned pause_scan, entered_scan, release_scan, abandon_scan;
static int guarded_ap_stop(void *user, uint32_t timeout_ms);
static int sta_scan(void *u, const h2_pal_wifi_scan_request_t *r,
    h2_pal_wifi_scan_result_fn f, void *c, uint32_t t) {
    (void)u; (void)r; (void)f; (void)c; (void)t;
    ++calls;
    assert(guarded_ap_stop(NULL, 0) == H2_PAL_ERR_BUSY);
    if (abandon_scan) {
        __atomic_store_n(&scan_phase, SCAN_ABANDONED, __ATOMIC_RELEASE);
        return H2_PAL_ERR_TIMEOUT;
    }
    if (pause_scan) {
        __atomic_store_n(&entered_scan, 1u, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&release_scan, __ATOMIC_ACQUIRE)) sched_yield();
    }
    return H2_PAL_ERR_IO;
}
static int sta_connect(void *u, const h2_pal_wifi_sta_config_t *c, uint32_t t) { (void)u; (void)c; (void)t; ++calls; return H2_PAL_ERR_IO; }
static int sta_disconnect(void *u) { (void)u; ++calls; return H2_PAL_ERR_IO; }
static int ap_start(void *u, const h2_pal_wifi_ap_config_t *c, uint32_t t) { (void)u; (void)c; (void)t; ++calls; return H2_PAL_ERR_IO; }
static int ap_stop(void *u, uint32_t t) { (void)u; (void)t; ++calls; return H2_PAL_ERR_IO; }
static int wifi_get_mac_address(void *u, uint8_t mac[6]) { (void)u; (void)mac; ++calls; return H2_PAL_ERR_IO; }
'''
        main = r'''
static void *scan_thread(void *unused) {
    (void)unused;
    assert(guarded_sta_scan(NULL, NULL, NULL, NULL, 0) == H2_PAL_ERR_IO);
    return NULL;
}
int main(void) {
    for (unsigned phase = SCAN_PENDING; phase <= SCAN_CLEANING; ++phase) {
        scan_phase = phase;
        assert(guarded_sta_disconnect(NULL) == H2_PAL_ERR_BUSY);
        assert(guarded_ap_start(NULL, NULL, 0) == H2_PAL_ERR_BUSY);
        assert(calls == 0 && wifi_operation_busy == 0);
    }
    scan_phase = SCAN_IDLE;
    assert(guarded_sta_scan(NULL, NULL, NULL, NULL, 0) == H2_PAL_ERR_IO);
    assert(calls == 1 && wifi_operation_busy == 0);
    assert(guarded_sta_connect(NULL, NULL, 0) == H2_PAL_ERR_IO);
    assert(guarded_sta_disconnect(NULL) == H2_PAL_ERR_IO);
    assert(guarded_ap_start(NULL, NULL, 0) == H2_PAL_ERR_IO);
    assert(guarded_ap_stop(NULL, 0) == H2_PAL_ERR_IO);
    assert(guarded_wifi_get_mac(NULL, NULL) == H2_PAL_ERR_IO);
    assert(calls == 6 && wifi_operation_busy == 0);
    pause_scan = 1;
    pthread_t worker;
    assert(pthread_create(&worker, NULL, scan_thread, NULL) == 0);
    while (!__atomic_load_n(&entered_scan, __ATOMIC_ACQUIRE)) sched_yield();
    assert(guarded_sta_disconnect(NULL) == H2_PAL_ERR_BUSY);
    assert(guarded_ap_start(NULL, NULL, 0) == H2_PAL_ERR_BUSY);
    __atomic_store_n(&release_scan, 1u, __ATOMIC_RELEASE);
    assert(pthread_join(worker, NULL) == 0);
    assert(calls == 7 && wifi_operation_busy == 0);
    assert(guarded_sta_disconnect(NULL) == H2_PAL_ERR_IO);
    assert(calls == 8 && wifi_operation_busy == 0);
    abandon_scan = 1;
    assert(guarded_sta_scan(NULL, NULL, NULL, NULL, 0) == H2_PAL_ERR_TIMEOUT);
    assert(wifi_operation_busy == 0 && scan_phase == SCAN_ABANDONED);
    assert(guarded_sta_connect(NULL, NULL, 0) == H2_PAL_ERR_BUSY);
    assert(guarded_ap_start(NULL, NULL, 0) == H2_PAL_ERR_BUSY);
    assert(guarded_sta_disconnect(NULL) == H2_PAL_ERR_BUSY);
    assert(calls == 9);
    scan_completed();
    assert(scan_phase == SCAN_REAPABLE && clears == 0);
    assert(guarded_sta_connect(NULL, NULL, 0) == H2_PAL_ERR_IO);
    assert(calls == 10 && wifi_operation_busy == 0);
    assert(clears == 1 && scan_phase == SCAN_IDLE);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / "operations.c"
            binary = Path(directory) / "operations-test"
            unit.write_text(fixture + guard + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                            *shlex.split(os.environ.get("JIELI_TEST_CFLAGS", "")), "-I", str(ROOT / "libs/pal/include"),
                            str(unit), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
