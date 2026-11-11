"""Exercise the actual reboot-monitor state machine with scripted transport I/O."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[5]
SOURCE = ROOT / "projects/testing/libs/h2loader_runner/src/h2_h2loader_e2e_runner.c"

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
static h2_pal_result_t monitor_output(void *, const uint8_t *, size_t);
static int connects, disconnects, monitors, statuses, windows, cancel_flag;
static int monitor_results[3], status_results[2], partitions[2];
static int reconnect_results[2], begin_results[3], close_results[3];
static int command_result, command_terminal;
static size_t initial_handshake_bytes, command_bytes;
static size_t handshake_bytes[2], window_bytes[3], status_bytes[2];
static void *monitor_connections[3];
static int cancelled(config_t *c) { (void)c; return cancel_flag; }
static void count_output(void *u) { (void)u; }
static int emit_logs(h2_e2e_transport_context_t *c, size_t size) {
  static const uint8_t bytes[8] = {'b', 'o', 'o', 't', '\n', 0, 42, 255};
  assert(size <= sizeof(bytes));
  return monitor_output(c, bytes, size);
}
static int connect_transport(h2_e2e_transport_context_t *c, h2_h2loader_host_status_t *s) {
  c->serial_connection=(void *)(uintptr_t)1;
  s->running_partition=1;
  return emit_logs(c, initial_handshake_bytes);
}
static int execute_command(h2_e2e_transport_context_t *c,
    const h2_h2loader_host_command_request_t *q, h2_h2loader_host_command_result_t *r) {
  (void)q; r->terminal=command_terminal;
  int rc=emit_logs(c, command_bytes);
  return rc == H2_PAL_OK ? command_result : rc;
}
static int disconnect_transport(h2_e2e_transport_context_t *c) {
  assert(disconnects<3);
  c->serial_connection=NULL;
  return close_results[disconnects++];
}
static int reconnect_after_reboot(h2_e2e_transport_context_t *c, uint32_t p,
    h2_h2loader_host_status_t *s) {
  /* Boot logs must be monitored on the ACK's connection before reconnecting. */
  assert(monitors>=1 && !c->monitor_logs && c->serial_connection==NULL);
  assert(connects<2);
  int index=connects++;
  c->serial_connection=(void *)(uintptr_t)(index+2);
  s->running_partition=p;
  int rc=emit_logs(c, handshake_bytes[index]);
  return rc == H2_PAL_OK ? reconnect_results[index] : rc;
}
static int begin_monitor_window(h2_e2e_transport_context_t *c) {
  assert(c->monitor_logs && c->serial_connection!=NULL && windows<3);
  return begin_results[windows++];
}
static int monitor_cancelled(void *user) { (void)user; return 1; }
static int h2_h2loader_host_serial_monitor_logs(void *connection,
    int (*cancel_fn)(void *), void *user) {
  h2_e2e_transport_context_t *c = user;
  assert(connection == c->serial_connection && connection!=NULL);
  assert(cancel_fn == monitor_cancelled && c->monitor_logs && monitors<3);
  int index=monitors++;
  monitor_connections[index]=connection;
  int rc=emit_logs(c, window_bytes[index]);
  return rc == H2_PAL_OK ? monitor_results[index] : rc;
}
static int read_status(h2_e2e_transport_context_t *c, h2_h2loader_host_status_t *s) {
  assert(statuses<2 && !c->monitor_logs && c->serial_connection!=NULL);
  int index=statuses++;
  s->running_partition=(uint32_t)partitions[index];
  int rc=emit_logs(c, status_bytes[index]);
  return rc == H2_PAL_OK ? status_results[index] : rc;
}
static void reset(void) {
  connects=disconnects=monitors=statuses=windows=cancel_flag=0;
  command_result=H2_PAL_OK;
  command_terminal=H2_H2LOADER_HOST_COMMAND_TERMINAL_OK;
  initial_handshake_bytes=command_bytes=0;
  for (size_t i=0; i<3; ++i) {
    monitor_results[i]=H2_PAL_EXIT;
    window_bytes[i]=1;
  }
  memset(handshake_bytes,0,sizeof(handshake_bytes));
  memset(status_bytes,0,sizeof(status_bytes));
  memset(status_results,0,sizeof(status_results));
  memset(reconnect_results,0,sizeof(reconnect_results));
  memset(begin_results,0,sizeof(begin_results));
  memset(close_results,0,sizeof(close_results));
  memset(monitor_connections,0,sizeof(monitor_connections));
  partitions[0]=partitions[1]=1;
}
"""

MAIN = r"""
int main(void) {
#define RESET_CASE() do { \
  reset(); \
  result = (result_t){0}; \
  ctx.serial_connection = NULL; \
  ctx.monitor_output_bytes = 0; \
  ctx.monitor_logs = 0; \
} while (0)
  config_t cfg={0}; result_t result={0};
  h2_e2e_transport_context_t ctx={.config=&cfg,.case_result=&result};

  /* A quiet firmware still passes when its boot logs were actually monitored. */
  RESET_CASE(); window_bytes[0]=3; window_bytes[1]=0;
  handshake_bytes[0]=8; status_bytes[0]=8;
  initial_handshake_bytes=command_bytes=8;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_OK);
  assert(connects==1 && monitors==2 && windows==2 && statuses==1);
  assert(monitor_connections[0]==(void *)(uintptr_t)1);
  assert(monitor_connections[1]==(void *)(uintptr_t)2);
  assert(result.status_valid && result.status.running_partition==1);
  assert(result.log_bytes==3 && ctx.monitor_output_bytes==3);
  assert(ctx.serial_connection==NULL && !ctx.monitor_logs);

  /* One reset can retire the original session while boot output is arriving. */
  RESET_CASE(); monitor_results[0]=H2_PAL_ERR_CLOSED;
  window_bytes[0]=3; window_bytes[1]=0;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_OK);
  assert(connects==1 && monitors==2 && statuses==1 && result.status_valid);
  RESET_CASE(); monitor_results[0]=H2_PAL_ERR_TIMEOUT;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_OK && connects==1);

  /* A reset deferred past the first window gets one bounded recovery. */
  RESET_CASE(); monitor_results[1]=H2_PAL_ERR_CLOSED; window_bytes[2]=0;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_OK);
  assert(connects==2 && monitors==3 && statuses==1 && result.status_valid);
  RESET_CASE(); status_results[0]=H2_PAL_ERR_TIMEOUT; window_bytes[2]=0;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_OK);
  assert(connects==2 && monitors==3 && statuses==2 && result.status_valid);

  /* Repeated transitions and a wrong live partition fail closed. */
  RESET_CASE(); monitor_results[0]=monitor_results[1]=H2_PAL_ERR_CLOSED;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_CLOSED && connects==1);
  assert(!result.status_valid && !ctx.monitor_logs);
  RESET_CASE(); monitor_results[0]=H2_PAL_ERR_CLOSED;
  status_results[0]=H2_PAL_ERR_TIMEOUT;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_TIMEOUT && connects==1);
  assert(!result.status_valid);
  RESET_CASE(); status_results[0]=status_results[1]=H2_PAL_ERR_TIMEOUT;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_TIMEOUT && connects==2);
  assert(!result.status_valid);
  RESET_CASE(); partitions[0]=2;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_INVALID_STATE && connects==1);
  assert(!result.status_valid);

  /* Initial/command/reconnect/status diagnostics never stand in for monitoring. */
  RESET_CASE(); window_bytes[0]=window_bytes[1]=window_bytes[2]=0;
  initial_handshake_bytes=command_bytes=handshake_bytes[0]=status_bytes[0]=8;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_NOT_FOUND);
  assert(monitors==2 && statuses==1 && !result.status_valid);
  assert(ctx.monitor_output_bytes==0 && result.log_bytes==0);
  RESET_CASE(); window_bytes[0]=window_bytes[1]=window_bytes[2]=0;
  status_results[0]=H2_PAL_ERR_TIMEOUT;
  handshake_bytes[0]=handshake_bytes[1]=8;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_NOT_FOUND && connects==2);
  assert(!result.status_valid && ctx.monitor_output_bytes==0);

  /* Errors and cancellation do not turn into retry loops or success. */
  RESET_CASE(); cancel_flag=1;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_EXIT);
  assert(connects==0 && !result.status_valid && !ctx.monitor_logs);
  RESET_CASE(); monitor_results[0]=H2_PAL_ERR_IO;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_IO && connects==0);
  RESET_CASE(); begin_results[0]=H2_PAL_ERR_IO;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_IO && monitors==0);
  RESET_CASE(); reconnect_results[0]=H2_PAL_ERR_IO;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_IO && monitors==1);
  RESET_CASE(); command_terminal=0;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_IO && monitors==0);
  RESET_CASE(); close_results[1]=H2_PAL_ERR_IO;
  assert(run_reboot_monitor(&ctx,1,1)==H2_PAL_ERR_IO);
  assert(ctx.serial_connection==NULL && !ctx.monitor_logs);

  check_monitor_output();
#undef RESET_CASE
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
            finish_start = re.search(r"\bstatic\s+h2_pal_result_t\s+finish_bounded_monitor\s*\(", source).start()
            finish_end = source.index("static h2_pal_result_t run_monitor(", finish_start)
            test.write_text(STUBS + source[log_start:log_end] + LOG_MAIN +
                            source[finish_start:finish_end] + source[start:end] + MAIN)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
