"""Exercise timeout ownership and late Wi-Fi scan result cleanup."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class WifiScanTest(unittest.TestCase):
    def test_ap_status_ssid_is_terminated_at_declared_length(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/"
                  "h2_jieli_ac791n_devkit_wifi.c").read_text()
        begin = source.index("  memcpy(wifi_state.ap.ssid,")
        copy = source[begin:source.index("  if (wifi_enter_ap_mode", begin)]
        program = r'''
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
static struct { struct { char ssid[33]; } ap; } wifi_state;
typedef struct { char ssid[33]; unsigned ssid_len; } config_t;
static void copy_status(const config_t *config) {
  wifi_state_lock();
'''+copy+r'''
}
int main(void) {
  config_t config;
  for (unsigned length=1;length<=32;++length) {
    memset(&config, 'X', sizeof(config));
    config.ssid_len=length;
    memset(wifi_state.ap.ssid, 'Y', sizeof(wifi_state.ap.ssid));
    copy_status(&config);
    assert(memcmp(wifi_state.ap.ssid,config.ssid,length)==0);
    assert(wifi_state.ap.ssid[length]==0);
    assert(strlen(wifi_state.ap.ssid)==length);
    if (length<32) assert(wifi_state.ap.ssid[length+1]=='Y');
  }
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.c"
            path.write_text(program)
            binary = Path(directory) / "test"
            subprocess.run([os.environ.get("CC", "cc"), *shlex.split(os.environ.get("JIELI_TEST_CFLAGS", "")), "-std=c11", "-Wall", "-Wextra", "-Werror", str(path),
                            "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_start_binds_events_for_existing_interface(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/"
                  "h2_jieli_ac791n_devkit_wifi.c").read_text()
        start = source[source.index("static int ensure_wifi_on(void)"):
                       source.index("static void update_sta_snapshot(", source.index("static int ensure_wifi_on(void)"))]
        stub = r'''
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

#define H2_PAL_OK 0
#define H2_PAL_ERR_IO -1
static struct { int on; } wifi_state;
static int sdk_on, registrations, starts, start_error, stack_starts;
static void h2_jieli_net_stack_started(void) {
 assert(sdk_on && !wifi_state.on); ++stack_starts;
}
static void wifi_event(void) {}
static void (*callback)(void);
static void wifi_set_event_callback(void (*cb)(void)) {
 callback=cb; ++registrations;
}
static int wifi_is_on(void) { return sdk_on; }
static int wifi_on(void) {
 assert(callback==wifi_event); ++starts;
 if(start_error) return start_error;
 sdk_on=1; return 0;
}
'''
        main = r'''
int main(void) {
 sdk_on=1;
 assert(ensure_wifi_on()==0);
 assert(callback==wifi_event && registrations==1 && starts==0);
 assert(ensure_wifi_on()==0 && registrations==1 && stack_starts==1);
 /* Model a successful stop, then a cold start. */
 sdk_on=0; wifi_state.on=0;
 assert(ensure_wifi_on()==0 && registrations==2 && starts==1 && stack_starts==2);
 assert(ensure_wifi_on()==0 && registrations==2 && starts==1 && stack_starts==2);
 sdk_on=0; wifi_state.on=0; start_error=-1;
 assert(ensure_wifi_on()==H2_PAL_ERR_IO && !wifi_state.on && stack_starts==2);
 start_error=0;
 assert(ensure_wifi_on()==0 && wifi_state.on && starts==3 && stack_starts==3);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-wifi-start-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + start + main)
            binary = Path(directory) / "test"
            subprocess.run([os.environ.get("CC", "cc"), *shlex.split(os.environ.get("JIELI_TEST_CFLAGS", "")), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_timeout_is_cleaned_only_after_completion(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/"
                  "h2_jieli_ac791n_devkit_wifi.c").read_text()
        event = source[source.index("static int wifi_event("):
                       source.index("static int ensure_wifi_on(void)")]
        self.assertRegex(event, r"WIFI_EVENT_SMP_CFG_START\)\s+scan_reset_after_sta_exit\(\)")
        state = source[source.index("enum { SCAN_IDLE"):
                       source.index("static void post_system_event")]
        if "static void scan_reap_completed(void)" not in state:
            state += "\nstatic void scan_reap_completed(void) {}\n"
        scan = source[source.index("static int sta_scan("):
                      source.index("static int sta_connect(")]
        status = source[source.index("static int sta_get_status("):
                        source.index("static int sta_scan(")]
        stub = r'''
#include "h2/pal/hal/h2_pal_wifi.h"
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
static void scan_completed(void);
static void deliver_completion(void);
static int in_sdk_callback;
static unsigned now, clears, requests, delivered;
static struct { int on; h2_pal_wifi_sta_status_t sta; } wifi_state;
static int request_error, complete_on_delay, complete_immediately;
#define STA_MODE 1
struct wifi_mode_info { int mode; };
static int mode = STA_MODE;
static inline void wifi_get_mode_cur_info(struct wifi_mode_info *info) { info->mode = mode; }
static unsigned frees;
static int keep_receiving;

struct wifi_scan_ssid_info {
 unsigned ssid_len; char ssid[33]; unsigned char mac_addr[6];
 int channel_number, rssi, auth_mode;
};
static struct wifi_scan_ssid_info item={.ssid_len=3,.ssid="abc"};
static inline void fake_free(void *buffer) { assert(buffer == &item); ++frees; }
#define free fake_free
static int ensure_wifi_on(void) { return H2_PAL_OK; }
static uint32_t timer_get_ms(void) { return now; }
static void os_time_dly(unsigned ticks) {
 assert(ticks==1); now+=10; if(complete_on_delay) deliver_completion();
}
static int wifi_scan_req(void) {
 ++requests;
 if(complete_immediately) deliver_completion();
 return request_error;
}
static void wifi_clear_scan_result(void) { assert(!in_sdk_callback); ++clears; }
static struct wifi_scan_ssid_info *wifi_get_scan_result(uint32_t *count) {
 *count=1; return &item;
}
static h2_pal_wifi_security_t map_security(int mode) {
 (void)mode; return H2_PAL_WIFI_SECURITY_OPEN;
}
static bool receive(void *user,const h2_pal_wifi_scan_entry_t *entry) {
 (void)user; assert(entry->ssid_len==3); ++delivered; return keep_receiving;
}
'''
        main = r'''
static void deliver_completion(void) {
 in_sdk_callback=1;
 scan_completed();
 in_sdk_callback=0;
}
int main(void) {
 h2_pal_wifi_sta_status_t status;
 (void)wifi_get_mode_cur_info; (void)fake_free;
 mode=0;
 assert(sta_scan(NULL,NULL,receive,NULL,0)==H2_PAL_ERR_INVALID_STATE);
 assert(requests==0 && scan_phase==SCAN_IDLE && clears==0 && frees==0);
 mode=STA_MODE;
 wifi_state.on=1;
 wifi_state.sta.state=H2_PAL_WIFI_STA_STATE_GOT_IP;
 wifi_state.sta.ip_valid=1;
 assert(sta_scan(NULL,NULL,receive,NULL,0)==H2_PAL_ERR_TIMEOUT);
 assert(sta_get_status(NULL,&status)==0 && status.state==H2_PAL_WIFI_STA_STATE_GOT_IP);
 assert(status.ip_valid && wifi_state.sta.state==H2_PAL_WIFI_STA_STATE_GOT_IP);
 assert(clears==0 && requests==1 && scan_phase==SCAN_ABANDONED);
 assert(sta_scan(NULL,NULL,receive,NULL,10)==H2_PAL_ERR_BUSY && requests==1);
 deliver_completion();
 assert(clears==0 && delivered==0);
 scan_reap_completed();
 assert(clears==1 && scan_phase==SCAN_IDLE);
 assert(sta_get_status(NULL,&status)==0 && status.state==H2_PAL_WIFI_STA_STATE_GOT_IP);
 scan_phase=SCAN_PENDING;
 assert(sta_get_status(NULL,&status)==0 && status.state==H2_PAL_WIFI_STA_STATE_SCANNING);
 wifi_state.sta.state=H2_PAL_WIFI_STA_STATE_DISCONNECTED;
 deliver_completion(); scan_phase=SCAN_IDLE;
 assert(sta_get_status(NULL,&status)==0 && status.state==H2_PAL_WIFI_STA_STATE_DISCONNECTED);
 deliver_completion(); assert(clears==1); /* duplicate/unowned completion */
 complete_on_delay=1;
 assert(sta_scan(NULL,NULL,receive,NULL,20)==H2_PAL_OK);
 assert(clears==2 && delivered==1 && frees==1 && scan_phase==SCAN_IDLE);
 complete_on_delay=0; request_error=-1;
 assert(sta_scan(NULL,NULL,receive,NULL,20)==H2_PAL_ERR_BUSY);
 assert(clears==2 && scan_phase==SCAN_IDLE);
 request_error=0; complete_immediately=1; keep_receiving=1;
 assert(sta_scan(NULL,NULL,receive,NULL,0)==H2_PAL_OK);
 assert(clears==3 && delivered==2 && frees==2 && scan_phase==SCAN_IDLE);
 complete_immediately=0; unsigned before=now;
 assert(sta_scan(NULL,NULL,receive,NULL,9)==H2_PAL_ERR_TIMEOUT && before==now);
 deliver_completion();
 assert(clears==3);
 scan_reap_completed();
 assert(clears==4 && scan_phase==SCAN_IDLE);
 unsigned clears_before_reset=clears;
 scan_phase=SCAN_ABANDONED;
 in_sdk_callback=1;
 scan_reset_after_sta_exit();
 in_sdk_callback=0;
 assert(scan_phase==SCAN_IDLE && clears==clears_before_reset);
 complete_on_delay=1;
 assert(sta_scan(NULL,NULL,receive,NULL,20)==H2_PAL_OK);
 clears_before_reset=clears;
 scan_phase=SCAN_REAPABLE;
 in_sdk_callback=1;
 scan_reset_after_sta_exit();
 assert(scan_phase==SCAN_IDLE && clears==clears_before_reset);
 scan_phase=SCAN_PENDING;
 scan_reset_after_sta_exit();
 assert(scan_phase==SCAN_PENDING && clears==clears_before_reset);
 scan_phase=SCAN_CLEANING;
 scan_reset_after_sta_exit();
 assert(scan_phase==SCAN_CLEANING && clears==clears_before_reset);
 scan_phase=SCAN_READY;
 scan_reset_after_sta_exit();
 assert(scan_phase==SCAN_READY && clears==clears_before_reset);
 scan_phase=SCAN_IDLE;
 scan_reset_after_sta_exit();
 assert(scan_phase==SCAN_IDLE && clears==clears_before_reset);
 in_sdk_callback=0;
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-wifi-scan-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + state + status + scan + main)
            binary = Path(directory) / "test"
            subprocess.run([os.environ.get("CC", "cc"), *shlex.split(os.environ.get("JIELI_TEST_CFLAGS", "")), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "libs/pal/include"), str(test),
                            "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
