"""Execute the boot restore against read/connect-only PAL stubs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / "projects/example/native_component_src/jieli/wl82/devkit_app/src/color_bar_pal.c"
STUB = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#define H2_PAL_OK 0
#define H2_PAL_ERR_NOT_FOUND -8
typedef struct { char ssid[33], password[65]; size_t ssid_len, password_len; } h2_pal_wifi_sta_config_t;
static int read_result, connect_result, connects, lines;
static char output[256];
static const void *h2_jieli_ac791n_devkit_wifi_settings_api(void) { return (void *)1; }
static const void *h2_jieli_ac791n_devkit_wifi_sta_api(void) { return (void *)2; }
static int h2_pal_wifi_settings_get_saved_sta_config(const void *api, h2_pal_wifi_sta_config_t *out) {
  assert(api == (void *)1);
  strcpy(out->ssid, "test-network"); out->ssid_len = 12;
  strcpy(out->password, "placeholder-password"); out->password_len = 20;
  return read_result;
}
static int h2_pal_wifi_sta_connect(const void *api, const h2_pal_wifi_sta_config_t *saved, uint32_t timeout) {
  assert(api == (void *)2 && timeout == 0u);
  assert(strcmp(saved->ssid, "test-network") == 0);
  assert(strcmp(saved->password, "placeholder-password") == 0);
  ++connects; return connect_result;
}
static void usb_write_status(const char *format, ...) {
  va_list args; va_start(args, format);
  vsnprintf(output, sizeof(output), format, args); va_end(args); ++lines;
}
'''
MAIN = r'''
int main(void) {
  restore_saved_wifi();
  assert(connects == 1 && lines == 1);
  assert(strcmp(output, "JIELI_WIFI_RESTORE saved=1 result=0 ssid=test-network\r\n") == 0);
  assert(strstr(output, "placeholder-password") == NULL);
  connects = lines = 0; read_result = H2_PAL_ERR_NOT_FOUND;
  restore_saved_wifi();
  assert(connects == 0 && lines == 1);
  assert(strcmp(output, "JIELI_WIFI_RESTORE saved=0 result=-8 ssid=-\r\n") == 0);
  connects = lines = 0; read_result = -4;
  restore_saved_wifi();
  assert(connects == 0 && lines == 1);
  assert(strcmp(output, "JIELI_WIFI_RESTORE saved=0 result=-4 ssid=-\r\n") == 0);
  connects = lines = 0; read_result = 0; connect_result = -4;
  restore_saved_wifi();
  assert(connects == 1 && lines == 1);
  assert(strcmp(output, "JIELI_WIFI_RESTORE saved=1 result=-4 ssid=test-network\r\n") == 0);
  return 0;
}
'''


class WifiRestoreTest(unittest.TestCase):
    def test_restore_is_read_only_and_nonblocking(self):
        source = APP.read_text()
        begin = source.index("/* restore_saved_wifi begin */")
        end = source.index("/* restore_saved_wifi end */", begin)
        # No settings-write stub: any accidental write also fails to link.
        with tempfile.TemporaryDirectory(prefix="h2-wifi-restore-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(STUB + source[begin:end] + MAIN)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Werror", str(test), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_restore_precedes_transport_and_app_branches(self):
        source = APP.read_text().split("void app_main(void)", 1)[1]
        self.assertEqual(source.count("restore_saved_wifi();"), 1)
        self.assertLess(source.index("h2_loader_app_client_init("),
                        source.index("restore_saved_wifi();"))
        self.assertLess(source.index("restore_saved_wifi();"),
                        source.index("h2_jieli_app_iostreamikcp_start("))


if __name__ == "__main__":
    unittest.main()
