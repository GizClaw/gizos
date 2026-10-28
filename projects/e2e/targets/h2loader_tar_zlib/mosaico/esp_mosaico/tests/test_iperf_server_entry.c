#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
static uint64_t clock_ms;
static int clock_error;
static int fake_monotonic(uint64_t* out) {
  *out = clock_ms;
  return clock_error;
}
#define H2_MOSAICO_SERVER_HOST_TEST 1
#define H2_MOSAICO_SERVER_NOW(out) fake_monotonic(out)
#include "projects/e2e/targets/h2loader_tar_zlib/mosaico/esp_mosaico/tests/iperf_server_under_test.h"
static h2_iperf_server_app_snapshot_t snapshot;
static int snapshot_error, request_error;
static unsigned requests;
static struct {
  unsigned mode;
  bool running;
} request_log[12];
int h2_iperf_server_app_snapshot(h2_iperf_server_app_t* instance,
                                 h2_iperf_server_app_snapshot_t* out) {
  assert(instance == app);
  *out = snapshot;
  return snapshot_error;
}
int h2_iperf_server_app_request(h2_iperf_server_app_t* instance,
                                h2_iperf_server_app_mode_t mode, bool running) {
  assert(instance == app && requests < 12);
  request_log[requests].mode = mode;
  request_log[requests++].running = running;
  return request_error;
}
static void reset(void) {
  memset(&snapshot, 0, sizeof(snapshot));
  memset(statuses, 0, sizeof(statuses));
  clock_ms = phase_started = completed_at = stop_started = 0;
  clock_error = snapshot_error = request_error = first_error = 0;
  phase_index = requests = 0;
  phase_ready = packets_complete = stop_requested = false;
  app = (h2_iperf_server_app_t*)(uintptr_t)1;
  runtime = NULL;
  assert(ready(NULL) == 0 && statuses[0] == 1);
  assert(requests == 1 && request_log[0].mode == 4 && request_log[0].running);
}
static void complete(unsigned mode, uint32_t count4, uint32_t count6) {
  snapshot.mode = mode;
  snapshot.phase = H2_IPERF_SERVER_APP_LISTENING;
  snapshot.streams[0].completed_tests = count4;
  snapshot.streams[1].completed_tests = count6;
  assert(!should_exit(NULL));
  clock_ms += 60000;
  assert(!should_exit(NULL));
  assert(stop_requested && !request_log[requests - 1].running);
  assert(statuses[phase_index + 1] == 0); /* Not PASS until stop completes. */
  snapshot.phase = H2_IPERF_SERVER_APP_STOPPING;
  assert(!should_exit(NULL));
  snapshot.phase = H2_IPERF_SERVER_APP_STOPPED;
  assert(should_exit(NULL) == (mode == 46));
}
int main(void) {
  reset();
  snapshot.mode = H2_IPERF_SERVER_APP_MODE_IPV4;
  snapshot.phase = H2_IPERF_SERVER_APP_LISTENING;
  snapshot.streams[0].completed_tests = 29;
  assert(!should_exit(NULL));
  clock_ms = 90000;
  assert(!should_exit(NULL) && !packets_complete && requests == 1);
  snapshot.streams[0].completed_tests = 30;
  snapshot.streams[0].active = true;
  assert(!should_exit(NULL) && !packets_complete);
  snapshot.streams[0].active = false;
  complete(4, 30, 0);
  assert(statuses[1] == 1 && phase_index == 1);
  assert(request_log[requests - 1].mode == 6 &&
         request_log[requests - 1].running);
  snapshot.mode = H2_IPERF_SERVER_APP_MODE_IPV6;
  snapshot.phase = H2_IPERF_SERVER_APP_LISTENING;
  snapshot.streams[0].completed_tests = 100;
  snapshot.streams[1].completed_tests = 29;
  assert(!should_exit(NULL) && !packets_complete);
  complete(6, 0, 30);
  snapshot.mode = H2_IPERF_SERVER_APP_MODE_DUAL;
  snapshot.phase = H2_IPERF_SERVER_APP_LISTENING;
  snapshot.streams[0].completed_tests = 30;
  snapshot.streams[1].completed_tests = 29;
  assert(!should_exit(NULL) && !packets_complete);
  complete(46, 30, 30);
  assert(phase_index == 3 && statuses[3] == 1 && statuses[4] == 0);
  assert(app != NULL); /* Caller still owns UI/controller teardown. */

  reset();
  snapshot.mode = H2_IPERF_SERVER_APP_MODE_IPV4;
  snapshot.phase = H2_IPERF_SERVER_APP_LISTENING;
  snapshot.streams[0].completed_tests = 30;
  assert(!should_exit(NULL));
  clock_ms += 60000;
  assert(!should_exit(NULL));
  snapshot.phase = H2_IPERF_SERVER_APP_STOPPING;
  snapshot.error = H2_PAL_ERR_IO;
  unsigned before = requests;
  assert(should_exit(NULL) && statuses[1] == 2 && statuses[2] == 0);
  assert(should_exit(NULL) && requests == before && app != NULL);

  reset();
  clock_ms = 650000;
  assert(should_exit(NULL) && first_error == H2_PAL_ERR_TIMEOUT);
  reset();
  snapshot_error = H2_PAL_ERR_IO;
  assert(should_exit(NULL) && statuses[1] == 2);
  return 0;
}
