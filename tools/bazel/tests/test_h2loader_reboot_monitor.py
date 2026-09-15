"""Exercise the actual reboot-monitor state machine with scripted transport I/O."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "projects/h2loader/apps/e2e-runner/src/h2_h2loader_e2e_runner.c"

STUBS = r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef int h2_pal_result_t;
typedef int h2_h2loader_host_command_t;
enum { H2_PAL_EXIT=2, H2_PAL_OK=0, H2_PAL_ERR_CLOSED=-1, H2_PAL_ERR_TIMEOUT=-2,
       H2_PAL_ERR_IO=-3, H2_PAL_ERR_INVALID_STATE=-4,
       H2_PAL_ERR_NOT_FOUND=-5, H2_PAL_ERR_INVALID_ARG=-6, H2_H2LOADER_HOST_COMMAND_TERMINAL_OK=1 };
typedef struct { uint32_t running_partition; } h2_h2loader_host_status_t;
typedef struct { int terminal; } h2_h2loader_host_command_result_t;
typedef struct {
  int command; void *status; int (*is_cancelled)(void *); void *cancel_user;
  void (*on_output)(void *); void *output_user;
} h2_h2loader_host_command_request_t;
typedef struct {
  int (*is_cancelled)(void *); void *cancel_user;
  int (*on_log)(void *, const uint8_t *, size_t); void *log_user;
} config_t;
typedef struct { int terminal, status_valid; size_t output_bytes, log_bytes; h2_h2loader_host_status_t status; } result_t;
typedef struct {
  void *serial_connection;
  int monitor_logs; size_t monitor_output_bytes; config_t *config; result_t *case_result;
} h2_e2e_transport_context_t;
static int connects, disconnects, monitors, statuses, windows, cancel_flag;
static int monitor_results[2], status_results[2], partitions[2];
static size_t handshake_bytes[2], window_bytes[2];
static int cancelled(config_t *c) { (void)c; return cancel_flag; }
static void count_output(void *u) { (void)u; }
static int connect_transport(h2_e2e_transport_context_t *c, h2_h2loader_host_status_t *s) {
  (void)c; s->running_partition=1; return 0;
}
static int execute_command(h2_e2e_transport_context_t *c,
    const h2_h2loader_host_command_request_t *q, h2_h2loader_host_command_result_t *r) {
  (void)c; (void)q; r->terminal=1; return 0;
}
static int disconnect_transport(h2_e2e_transport_context_t *c) { (void)c; ++disconnects; return 0; }
static int reconnect_after_reboot(h2_e2e_transport_context_t *c, uint32_t p,
    h2_h2loader_host_status_t *s) {
  assert(connects<2); c->monitor_output_bytes += handshake_bytes[connects++];
  s->running_partition=p; return 0;
}
static int begin_monitor_window(h2_e2e_transport_context_t *c) { (void)c; ++windows; return 0; }
static int monitor_cancelled(void *user) { (void)user; return 1; }
static int h2_h2loader_host_serial_monitor_logs(void *connection,
    int (*cancel_fn)(void *), void *user) {
  h2_e2e_transport_context_t *c = user;
  assert(connection == c->serial_connection && cancel_fn == monitor_cancelled);
  assert(monitors<2);
  c->monitor_output_bytes += window_bytes[monitors];
  return monitor_results[monitors++];
}
static int read_status(h2_e2e_transport_context_t *c, h2_h2loader_host_status_t *s) {
  (void)c; assert(statuses<2); s->running_partition=(uint32_t)partitions[statuses];
  return status_results[statuses++];
}
static void reset(void) {
  connects=disconnects=monitors=statuses=windows=cancel_flag=0;
  monitor_results[0]=monitor_results[1]=H2_PAL_EXIT;
  memset(handshake_bytes,0,sizeof(handshake_bytes));
  window_bytes[0]=window_bytes[1]=1;
  memset(status_results,0,sizeof(status_results));
  partitions[0]=partitions[1]=1;
}
"""

MAIN = r"""
int main(void) {
  /* Keep baseline extraction warning-clean for the fail-before check. */
  (void)cancelled; (void)read_status;
  config_t cfg={0}; result_t result={0};
  h2_e2e_transport_context_t ctx={.config=&cfg,.case_result=&result};
  reset(); status_results[0]=H2_PAL_ERR_TIMEOUT;
  assert(run_reboot_monitor(&ctx,1,1)==0);
  assert(connects==2 && windows==2 && monitors==2 && statuses==2);
  assert(disconnects==3 && result.status_valid && !ctx.monitor_logs);
  reset(); monitor_results[0]=H2_PAL_ERR_CLOSED;
  assert(run_reboot_monitor(&ctx,1,1)==0 && connects==2 && statuses==1);
  reset(); status_results[0]=status_results[1]=H2_PAL_ERR_TIMEOUT;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_TIMEOUT && connects==2);
  reset(); monitor_results[0]=monitor_results[1]=H2_PAL_ERR_CLOSED;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_CLOSED && connects==2);
  reset(); partitions[0]=2;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_INVALID_STATE && connects==1);
  reset(); window_bytes[0]=0;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_NOT_FOUND && connects==1);
  reset(); status_results[0]=H2_PAL_ERR_TIMEOUT; cancel_flag=1;
  monitor_results[0]=H2_PAL_OK;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_TIMEOUT && connects==1);
  reset(); handshake_bytes[0]=8; window_bytes[0]=0;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_NOT_FOUND && connects==1);
  assert(monitors==1 && statuses==0 && ctx.monitor_output_bytes==0);
  reset(); handshake_bytes[0]=8; window_bytes[0]=3;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_OK && connects==1);
  assert(monitors==1 && statuses==1 && result.status_valid &&
         ctx.monitor_output_bytes==3);
  check_monitor_output();
  return 0;
}
"""

LOG_MAIN = r"""
static size_t logged;
static int sink_result;
static const uint8_t payload[] = {0, 42, 255};
static int sink(void *user, const uint8_t *data, size_t len) {
  assert(user == &logged);
  assert(len == sizeof(payload) && memcmp(data, payload, len) == 0);
  logged += len;
  return sink_result;
}
static void check_monitor_output(void) {
  config_t cfg = {.on_log=sink, .log_user=&logged};
  result_t result = {0};
  h2_e2e_transport_context_t ctx = {.config=&cfg, .case_result=&result};
  assert(monitor_output(&ctx, payload, sizeof(payload)) == H2_PAL_OK);
  assert(logged == 3 && result.output_bytes == 0 && result.log_bytes == 0 &&
         ctx.monitor_output_bytes == 0);
  ctx.monitor_logs = 1;
  assert(monitor_output(&ctx, payload, sizeof(payload)) == H2_PAL_OK);
  assert(logged == 6 && result.output_bytes == 3 && result.log_bytes == 3 &&
         ctx.monitor_output_bytes == 3);
  ctx.monitor_logs = 0;
  sink_result = H2_PAL_ERR_IO;
  assert(monitor_output(&ctx, payload, sizeof(payload)) == H2_PAL_ERR_IO);
  assert(logged == 9 && result.output_bytes == 3 && result.log_bytes == 3 &&
         ctx.monitor_output_bytes == 3);
  cfg.on_log = NULL;
  assert(monitor_output(&ctx, payload, sizeof(payload)) == H2_PAL_OK);
  assert(monitor_output(&ctx, NULL, 0) == H2_PAL_OK);
  assert(monitor_output(NULL, payload, sizeof(payload)) == H2_PAL_ERR_INVALID_ARG);
  assert(monitor_output(&ctx, NULL, 1) == H2_PAL_ERR_INVALID_ARG);
  ctx.case_result = NULL;
  assert(monitor_output(&ctx, payload, sizeof(payload)) == H2_PAL_ERR_INVALID_ARG);
}
"""


class RebootMonitorTests(unittest.TestCase):
    def test_uart_reset_without_usb_disconnect_and_bounded_recovery(self):
        source = SOURCE.read_text()
        start = source.index("static h2_pal_result_t run_reboot_monitor(")
        end = source.index("\nstatic h2_pal_result_t run_reboot_preserves_stage(", start)
        with tempfile.TemporaryDirectory(prefix="h2-reboot-test-") as directory:
            test = Path(directory) / "test.c"
            log_start = source.index("static h2_pal_result_t monitor_output(")
            log_end = source.index("static int monitor_cancelled(", log_start)
            finish_start = source.index("static h2_pal_result_t\nfinish_bounded_monitor(")
            finish_end = source.index("static h2_pal_result_t run_monitor(", finish_start)
            test.write_text(STUBS + source[log_start:log_end] + LOG_MAIN +
                            source[finish_start:finish_end] + source[start:end] + MAIN)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
