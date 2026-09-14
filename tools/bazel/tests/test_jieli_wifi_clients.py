"""A truncated AP client listing must not truncate radio status."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class WifiClientsTest(unittest.TestCase):
    def test_output_capacity_does_not_change_total(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_wifi.c").read_text()
        begin = source.index("static int ap_get_clients(")
        function = source[begin:source.index("/* Serialize task-side", begin)]
        fixture = r'''
#include <assert.h>

static unsigned fake_state_gate;
static inline void wifi_state_lock(void) {
    assert(fake_state_gate == 0);
    fake_state_gate = 1;
}
static inline void wifi_state_unlock(void) {
    assert(fake_state_gate == 1);
    fake_state_gate = 0;
}

#include <string.h>
#include "h2/pal/hal/h2_pal_wifi.h"
static struct { h2_pal_wifi_ap_status_t ap; } wifi_state;
static int present = 1;
static int wifi_get_sta_entry_rssi(char id, char **rssi, uint8_t **evm, uint8_t **mac) {
    (void)evm;
    assert(fake_state_gate == 0);
    static char signal;
    static uint8_t address[6] = {2, 3, 4, 5, 6, 7};
    assert(id >= 0 && id < 8);
    signal = present && (id == 0 || id == 7) ? -42 : 0;
    *rssi = &signal;
    *mac = address;
    return 0;
}
'''
        main = r'''
int main(void) {
    h2_pal_wifi_ap_client_t clients[3];
    size_t count = 99;
    memset(clients, 0xa5, sizeof(clients));
    assert(ap_get_clients(NULL, clients, 1, &count) == H2_PAL_OK);
    assert(count == 1 && wifi_state.ap.client_count == 2);
    assert(clients[0].station_id == 0 && clients[0].rssi == -42);
    assert(clients[1].mac[0] == 0xa5);
    assert(ap_get_clients(NULL, NULL, 0, &count) == H2_PAL_OK);
    assert(count == 0 && wifi_state.ap.client_count == 2);
    assert(ap_get_clients(NULL, clients, 3, &count) == H2_PAL_OK);
    assert(count == 2 && clients[1].station_id == 7);
    present = 0;
    assert(ap_get_clients(NULL, clients, 3, &count) == H2_PAL_OK);
    assert(count == 0 && wifi_state.ap.client_count == 0);
    assert(ap_get_clients(NULL, NULL, 1, &count) == H2_PAL_ERR_INVALID_ARG);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(fixture + function + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "libs/pal/include"),
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
