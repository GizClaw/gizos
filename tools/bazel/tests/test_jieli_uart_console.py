"""Host regression for the real board UART console with mocked SDK I/O."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import re

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_console.c"

STUB = r"""
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
typedef unsigned char u8;
typedef unsigned int u32;
typedef int OS_MUTEX;
typedef int h2_pal_result_t;
enum { H2_PAL_OK=0, H2_PAL_ERR_IO=-10, H2_PAL_ERR_UNAVAILABLE=-11,
       H2_PAL_ERR_INVALID_ARG=-12, H2_PAL_ERR_TIMEOUT=-13,
       OS_NO_ERR=0, UART_RECV_TIMEOUT=-2, UART_CIRCULAR_BUFFER_WRITE_OVERLAY=-1,
       UART_SET_CIRCULAR_BUFF_ADDR=1, UART_SET_CIRCULAR_BUFF_LENTH,
       UART_SET_RECV_BLOCK, UART_START, UART_FLUSH };
static int handle, opens, closes, creates, locks, unlocks, held, lock_fail;
static int init_fail=1, ioctls, read_result, flushes, writes, short_write;
static unsigned char received[2048];
static size_t received_count;
static int os_mutex_create(OS_MUTEX *m) { ++creates; return 0; }
static int os_mutex_pend(OS_MUTEX *m, int ticks) {
  assert(ticks>0); ++locks;
  if (lock_fail) return 1;
  assert(!held); held=1; return 0;
}
static int os_mutex_post(OS_MUTEX *m) { assert(held); held=0; ++unlocks; return 0; }
static void *dev_open(const char *name, void *arg) {
  assert(strcmp(name,"uart1")==0); ++opens; return &handle;
}
static int dev_close(void *h) { assert(h==&handle); ++closes; return 0; }
static int dev_ioctl(void *h, int op, u32 arg) {
  assert(h==&handle); ++ioctls;
  if (init_fail) return -1;
  if (op==UART_SET_CIRCULAR_BUFF_LENTH) assert(arg==16*1024);
  if (op==UART_SET_RECV_BLOCK) assert(arg==0);
  if (op==UART_FLUSH) ++flushes;
  return 0;
}
static int dev_read(void *h, void *data, u32 size) {
  assert(h==&handle);
  if (read_result>0) memset(data,0xab,(size_t)read_result);
  return read_result;
}
static int dev_write(void *h, void *data, u32 size) {
  assert(h==&handle && held && size<=512);
  assert(((uintptr_t)data%32)==0);
  ++writes;
  if (short_write && size>17) size=17;
  memcpy(received+received_count,data,size); received_count+=size;
  return (int)size;
}
"""

MAIN = r"""
int main(void) {
  unsigned char data[1100], out[32];
  for (int i=0;i<1100;++i) data[i]=(unsigned char)i;
  assert(h2_jieli_ac791n_devkit_console_read(out,32)==H2_PAL_ERR_UNAVAILABLE);
  assert(h2_jieli_ac791n_devkit_console_start()==H2_PAL_ERR_IO);
  assert(closes==1 && creates==1);
  init_fail=0;
  assert(h2_jieli_ac791n_devkit_console_start()==0);
  assert(h2_jieli_ac791n_devkit_console_start()==0 && opens==2 && creates==1);
  int before=locks;
  assert(h2_jieli_ac791n_devkit_console_write(data+1,1003,100)==1003);
  assert(locks==before+1 && unlocks==1 && writes==2 && !held);
  assert(memcmp(received,data+1,1003)==0);
  received_count=0; short_write=1;
  assert(h2_jieli_ac791n_devkit_console_write(data,83,100)==83);
  assert(memcmp(received,data,83)==0 && !held);
  lock_fail=1; before=writes;
  assert(h2_jieli_ac791n_devkit_console_write(data,1,0)==H2_PAL_ERR_TIMEOUT);
  assert(writes==before);
  lock_fail=0;
  read_result=UART_RECV_TIMEOUT;
  assert(h2_jieli_ac791n_devkit_console_read(out,32)==0);
  read_result=UART_CIRCULAR_BUFFER_WRITE_OVERLAY;
  assert(h2_jieli_ac791n_devkit_console_read(out,32)==H2_PAL_ERR_IO && flushes==1);
  read_result=7;
  assert(h2_jieli_ac791n_devkit_console_read(out,32)==7 && out[6]==0xab);
  assert(h2_jieli_ac791n_devkit_console_write(NULL,1,1)==H2_PAL_ERR_INVALID_ARG);
  return 0;
}
"""


class UartConsoleTest(unittest.TestCase):
    def test_ble_smoke_uses_shared_layout_and_is_not_a_release(self):
        target = (ROOT / "projects/e2e/targets/h2loader_tar_zlib/"
                  "pal-ble-smoke/jieli_ac791n_devkit")
        build = (target / "BUILD.bazel").read_text()
        self.assertIn('h2loader_jieli_firmware(', build)
        self.assertIn('tags = ["no-release"]', build)
        self.assertNotIn('project_makefile =', build)
        self.assertNotIn('sdk_patches =', build)
        self.assertFalse((target / "project.mk").exists())
        self.assertFalse((target / "include/app_config.h").exists())
        self.assertFalse((ROOT / "projects/h2loader/targets/jieli_firmware/"
                         "ble-smoke/ac791n_devkit/BUILD.bazel").exists())
        source = (target / "src/pal_ble_smoke.c").read_text()
        self.assertIn("h2_jieli_app_iostreamikcp_start(", source)

    def test_shared_board_does_not_request_ble_bonding(self):
        board = ROOT / "boards/jieli_ac791n_devkit/ac791n"
        config = (board / "include/h2_jieli_ac791n_devkit_sdk_config.h").read_text()
        source = (board / "src/h2_jieli_ac791n_devkit_ble.c").read_text()
        self.assertRegex(config, r'#define TCFG_BLE_SECURITY_EN\s+0\b')
        self.assertIn('TCFG_BLE_SECURITY_EN ? SM_AUTHREQ_BONDING : 0', source)
        self.assertIn('sm_set_request_security(TCFG_BLE_SECURITY_EN)', source)

    def test_color_bars_follow_pal_dimensions(self):
        app = (ROOT / "projects/example/targets/h2loader_tar_zlib/display/"
               "jieli_ac791n_devkit/src/color_bar_pal.c").read_text()
        function = app[app.index("static int draw_color_bars("):
                       app.index("static int probe_sd_filesystem(")]
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
enum { H2_DISPLAY_OK=0, H2_DISPLAY_ERR_INVALID_ARG=-1,
       H2_DISPLAY_PIXEL_RGB565=0, H2_LCD_MAX_WIDTH=480 };
typedef int h2_pal_display_api_t;
typedef struct { int width, height; } h2_display_info_t;
typedef struct { int x, y, width, height; } h2_display_rect_t;
static uint16_t color_line[H2_LCD_MAX_WIDTH];
static int width, height, rows;
static int h2_pal_display_get_info(const int *d, h2_display_info_t *i) {
  i->width=width; i->height=height; return 0;
}
static int h2_pal_display_draw_bitmap(const int *d,
    const h2_display_rect_t *r, const void *p, size_t stride, int fmt) {
  assert(r->x==0 && r->y==rows && r->width==width && r->height==1);
  assert(r->y<height && stride >= width*2);
  assert(((const uint16_t *)p)[0]==0xffff);
  assert(((const uint16_t *)p)[width-1]==0);
  ++rows; return 0;
}
static int h2_pal_display_present(const int *d) { assert(rows==height); return 0; }
'''
        main = r'''
int main(void) {
  int display=0;
  width=480; height=320; assert(draw_color_bars(&display)==0);
  width=320; height=480; rows=0; assert(draw_color_bars(&display)==0);
  width=481; assert(draw_color_bars(&display)==-1);
  width=0; assert(draw_color_bars(&display)==-1);
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-color-size-test-") as directory:
            source = Path(directory) / "test.c"
            source.write_text(stub + function + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", str(source), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_ble_sdk_policy_matches_existing_board_apps(self):
        target = ROOT / "projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit/src"
        policies = [target / (name + "_task_policy.c")
                    for name in ("color_bar", "touch", "button", "audio_system")]
        policies += [
            ROOT / "projects/h2loader/targets/h2loader_tar_zlib/loader/"
                   "jieli_ac791n_devkit/src/loader_task_policy.c",
            ROOT / "projects/e2e/targets/h2loader_tar_zlib/pal-ble-smoke/"
                   "jieli_ac791n_devkit/src/pal_ble_smoke_task_policy.c",
        ]
        for task in ("#C0btctrler", "#C0btstack", "btctrler", "btstack"):
            pattern = r'\{"' + re.escape(task) + r'",\s*(\d+),\s*(\d+),\s*(\d+)\}'
            expected = re.search(pattern, policies[0].read_text()).groups()
            for path in policies[1:]:
                with self.subTest(task=task, policy=str(path)):
                    self.assertEqual(re.search(pattern, path.read_text()).groups(), expected)

    def test_mp4_task_policy_registers_shared_app_task_names(self):
        policy = (
            ROOT
            / "projects/example/targets/h2loader_tar_zlib/display/"
              "jieli_ac791n_devkit/src/mp4_player_small_task_policy.c"
        ).read_text()
        names = (
            ROOT
            / "projects/example/apps/mp4-player/app/include/"
              "h2_smoke_mp4_player_task_names.h"
        ).read_text()
        for macro in (
            "H2_SMOKE_MP4_PLAYER_AUDIO_TASK_NAME_VALUE",
            "H2_SMOKE_MP4_PLAYER_DECODER_TASK_NAME_VALUE",
        ):
            name = re.search(rf'#define {macro} "([^"]+)"', names).group(1)
            self.assertIn(f'{{"{name}",', policy)
        self.assertNotIn('{"mp4-decoder",', policy)

    def test_app_command_stack_matches_loader_policy(self):
        target = ROOT / "projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit/src"
        loader = ROOT / "projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit/src/loader_task_policy.c"
        pattern = r'\{(?:"h2loader/appcmd"|H2LOADER_APP_COMMAND_TASK_NAME_VALUE),\s*\d+,\s*(\d+),'
        loader_words = int(re.search(pattern, loader.read_text()).group(1))
        for name in ("color_bar_task_policy.c", "mp4_player_small_task_policy.c"):
            app_words = int(re.search(pattern, (target / name).read_text()).group(1))
            self.assertEqual(app_words, loader_words)
        transport = (target / "jieli_app_iostreamikcp.c").read_text()
        kib = int(re.search(r'H2_APP_COMMAND_STACK_SIZE = (\d+) \* 1024', transport).group(1))
        self.assertEqual(kib * 1024, loader_words * 4)

    def test_ble_session_has_a_distinct_registered_task_name(self):
        common = ROOT / "projects/h2loader/apps/loader/app"
        names = (common / "include/h2loader_app_task_names.h").read_text()
        self.assertIn('#define H2LOADER_BLE_COMMAND_TASK_NAME_VALUE "h2loader/blecmd"', names)
        self.assertIn('.name = h2loader_ble_command_task_name,',
                      (common / "h2loader_bleikcp.c").read_text())
        self.assertIn('.name = h2loader_app_command_task_name,',
                      (common / "app.c").read_text())
        target = ROOT / "projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit/src"
        self.assertIn('.task_name = H2LOADER_BLE_COMMAND_TASK_NAME_VALUE,',
                      (target / "jieli_app_ble.c").read_text())
        for name in ("color_bar", "touch", "button", "audio_system"):
            policy = (target / (name + "_task_policy.c")).read_text()
            for task in ("H2LOADER_BLE_COMMAND_TASK_NAME_VALUE",
                         "H2_BLEIKCP_SERVER_TASK_NAME_VALUE",
                         "H2_BLEIKCP_WORKER_TASK_NAME_VALUE",
                         "H2_LOADER_BLE_LINK_TASK_NAME_VALUE"):
                self.assertIn("{" + task + ",", policy)

    def test_app_boot_logs_before_command_transport(self):
        app = (ROOT / "projects/example/targets/h2loader_tar_zlib/display/"
               "jieli_ac791n_devkit/src/color_bar_pal.c").read_text()
        start = app.index("static void usb_write_status(const char *format, ...) {")
        end = app.index("static void report_previous_exception(void)", start)
        stub = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_H2_UART1_DEBUG_ENABLE
static char received[1024];
static unsigned calls;
static int h2_jieli_ac791n_devkit_console_write(
    const void *data, size_t size, uint32_t timeout) {
  assert(size < sizeof(received) && timeout == 100);
  memcpy(received, data, size); received[size] = 0; ++calls;
  return (int)size;
}
'''
        main = r'''
int main(void) {
  h2_jieli_wl82_boot_probe(100);
  assert(calls == 1 && strcmp(received, "JIELI_BOOT_STAGE stage=100\r\n") == 0);
  usb_write_status("JIELI_APP_INIT result=%d\r\n", -7);
  assert(calls == 2 && strstr(received, "result=-7") != NULL);
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-app-log-test-") as directory:
            source = Path(directory) / "test.c"
            source.write_text(stub + app[start:end] + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", str(source), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_mp4_early_logs_use_board_console(self):
        app = (ROOT / "projects/example/targets/h2loader_tar_zlib/display/"
               "jieli_ac791n_devkit/src/mp4_player_small_pal.c").read_text()
        probe_start = app.index("void h2_jieli_wl82_boot_probe(uint32_t stage)")
        probe_end = app.index("extern const char *os_current_task_rom", probe_start)
        probe = app[probe_start:probe_end]
        self.assertIn("H2_JIELI_MP4_BOOT_STAGE", probe)
        self.assertIn("h2_jieli_ac791n_devkit_console_write", probe)
        emit_start = app.index("static void emit(const char *format, ...)")
        emit_end = app.index("static int usb_log_write", emit_start)
        emit = app[emit_start:emit_end]
        self.assertIn("char status_line[320]", emit)
        self.assertIn("h2_jieli_ac791n_devkit_console_write", emit)
        self.assertNotIn("h2_jieli_app_iostreamikcp_log", emit)
        early_start = app.index(
            "int h2_jieli_ac791n_devkit_early_app_boot(void)")
        early_end = app.index("static h2_pal_result_t memory_stats_read", early_start)
        early_boot = app[early_start:early_end]
        self.assertNotIn("get_current_boot_info", early_boot)
        self.assertNotIn("enter_trial_boot(&is_trial)", early_boot)
        app_main = app[app.index("void app_main(void)"):]
        self.assertIn("enter_trial_boot(&is_trial)", app_main)

    def test_sdk_sequence_and_atomic_writes(self):
        source = "\n".join(line for line in SOURCE.read_text().splitlines()
                           if not line.startswith("#include"))
        with tempfile.TemporaryDirectory(prefix="h2-uart-test-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(STUB + source + MAIN)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wno-pointer-to-int-cast",
                            str(test), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
