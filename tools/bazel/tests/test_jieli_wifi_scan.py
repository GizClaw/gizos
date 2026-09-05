"""Exercise timeout ownership and late Wi-Fi scan result cleanup."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class WifiScanTest(unittest.TestCase):
    def test_timeout_is_cleaned_only_after_completion(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/"
                  "h2_jieli_ac791n_devkit_wifi.c").read_text()
        state = source[source.index("enum { SCAN_IDLE"):
                       source.index("static void post_system_event")]
        scan = source[source.index("static int sta_scan("):
                      source.index("static int sta_connect(")]
        stub = r'''
#include "h2/pal/hal/h2_pal_wifi.h"
#include <assert.h>
#include <string.h>
static void scan_completed(void);
static unsigned now, clears, requests, delivered;
static int request_error, complete_on_delay, complete_immediately;
struct wifi_scan_ssid_info {
 unsigned ssid_len; char ssid[33]; unsigned char mac_addr[6];
 int channel_number, rssi, auth_mode;
};
static struct wifi_scan_ssid_info item={.ssid_len=3,.ssid="abc"};
static int ensure_wifi_on(void) { return H2_PAL_OK; }
static uint32_t timer_get_ms(void) { return now; }
static void os_time_dly(unsigned ticks) {
 assert(ticks==1); now+=10; if(complete_on_delay) scan_completed();
}
static int wifi_scan_req(void) {
 ++requests; if(complete_immediately) scan_completed(); return request_error;
}
static void wifi_clear_scan_result(void) { ++clears; }
static struct wifi_scan_ssid_info *wifi_get_scan_result(uint32_t *count) {
 *count=1; return &item;
}
static h2_pal_wifi_security_t map_security(int mode) {
 (void)mode; return H2_PAL_WIFI_SECURITY_OPEN;
}
static bool receive(void *user,const h2_pal_wifi_scan_entry_t *entry) {
 (void)user; assert(entry->ssid_len==3); ++delivered; return 0;
}
'''
        main = r'''
int main(void) {
 assert(sta_scan(NULL,NULL,receive,NULL,0)==H2_PAL_ERR_TIMEOUT);
 assert(clears==0 && requests==1 && scan_phase==SCAN_ABANDONED);
 assert(sta_scan(NULL,NULL,receive,NULL,10)==H2_PAL_ERR_BUSY && requests==1);
 scan_completed(); assert(clears==1 && scan_phase==SCAN_IDLE && delivered==0);
 scan_completed(); assert(clears==1); /* duplicate/unowned completion */
 complete_on_delay=1;
 assert(sta_scan(NULL,NULL,receive,NULL,20)==H2_PAL_OK);
 assert(clears==2 && delivered==1 && scan_phase==SCAN_IDLE);
 complete_on_delay=0; request_error=-1;
 assert(sta_scan(NULL,NULL,receive,NULL,20)==H2_PAL_ERR_BUSY);
 assert(clears==2 && scan_phase==SCAN_IDLE);
 request_error=0; complete_immediately=1;
 assert(sta_scan(NULL,NULL,receive,NULL,0)==H2_PAL_OK);
 assert(clears==3 && delivered==2 && scan_phase==SCAN_IDLE);
 complete_immediately=0; unsigned before=now;
 assert(sta_scan(NULL,NULL,receive,NULL,9)==H2_PAL_ERR_TIMEOUT && before==now);
 scan_completed(); assert(clears==4 && scan_phase==SCAN_IDLE);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-wifi-scan-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + state + scan + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "libs/pal/include"), str(test),
                            "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
