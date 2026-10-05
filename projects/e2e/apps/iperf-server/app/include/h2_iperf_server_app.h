#ifndef H2_IPERF_SERVER_APP_H
#define H2_IPERF_SERVER_APP_H

#include "h2_iperf.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_IPERF_SERVER_APP_SSID "GizOS-iPerf"
#define H2_IPERF_SERVER_APP_PASSWORD "gizosiperf"
#define H2_IPERF_SERVER_APP_IPV4_TEXT "192.168.4.1"
#define H2_IPERF_SERVER_APP_IPV6_TEXT "fd53:697a:6f73:626::1"

typedef enum h2_iperf_server_app_mode {
  H2_IPERF_SERVER_APP_MODE_IPV4 = 4,
  H2_IPERF_SERVER_APP_MODE_IPV6 = 6,
  H2_IPERF_SERVER_APP_MODE_DUAL = 46,
} h2_iperf_server_app_mode_t;

typedef enum h2_iperf_server_app_phase {
  H2_IPERF_SERVER_APP_STOPPED,
  H2_IPERF_SERVER_APP_STARTING,
  H2_IPERF_SERVER_APP_LISTENING,
  H2_IPERF_SERVER_APP_STOPPING,
  H2_IPERF_SERVER_APP_ERROR,
} h2_iperf_server_app_phase_t;

/** Actual addresses and bind constraints supplied by the network owner.
 * Only SOURCE_ADDR constraints are retained, so this value has no borrowed
 * interface pointer. The disabled family's fields remain zero/empty.
 */
typedef struct h2_iperf_server_app_network {
  h2_pal_net_addr_t ipv4;
  h2_pal_net_addr_t ipv6;
  char ipv4_text[48];
  char ipv6_text[48];
} h2_iperf_server_app_network_t;

typedef struct h2_iperf_server_app_stream {
  bool active;
  h2_iperf_progress_t progress;
  h2_iperf_result_t result;
  h2_pal_result_t result_code;
  uint32_t completed_tests;
} h2_iperf_server_app_stream_t;

/** Value snapshot copied under the App's mutex; index 0 is IPv4, 1 IPv6. */
typedef struct h2_iperf_server_app_snapshot {
  h2_iperf_server_app_mode_t mode;
  h2_iperf_server_app_phase_t phase;
  h2_pal_result_t error;
  uint16_t port;
  h2_iperf_server_app_network_t network;
  h2_iperf_server_app_stream_t streams[2];
} h2_iperf_server_app_snapshot_t;

/** Borrowed network lifecycle hooks, called serially on the manager task.
 * start must return actual ready addresses and unwind all resources on error.
 * stop runs only after both server tasks have joined; it must tolerate retries.
 * Hooks must not change saved STA credentials and must not reenter this App.
 */
typedef struct h2_iperf_server_app_config {
  void *network_user;
  h2_pal_result_t (*network_start)(void *user, h2_iperf_server_app_mode_t mode,
                                   h2_iperf_server_app_network_t *out_network);
  h2_pal_result_t (*network_stop)(void *user);
  /** Zero selects the standard iperf3 port 5201. */
  uint16_t port;
  /** Optional readiness hook after Touch opens and the first Display flush
   * completes. Called once on the UI task; an error unwinds the UI.
   */
  h2_pal_result_t (*ui_ready)(void *user);
  void *ui_user;
} h2_iperf_server_app_config_t;

typedef struct h2_iperf_server_app h2_iperf_server_app_t;

/** Create a stopped App and its manager task. Borrow Runtime/hooks through
 * successful destroy. The controller uses Memory/Net/Time/Task/Sync PAL only.
 */
h2_pal_result_t
h2_iperf_server_app_create(h2_runtime_t *runtime,
                           const h2_iperf_server_app_config_t *config,
                           h2_iperf_server_app_t **out_app);
/** Thread-safe nonblocking request. Mode changes require STOPPED or ERROR.
 * Stop cancels idle accepts, incomplete handshakes and active measurements.
 * A cleanup failure retains STOPPING and its error with live handles owned;
 * another Stop request retries cleanup. Start/mode changes stay busy until
 * cleanup succeeds. A fatal manager synchronization error closes requests;
 * destroy still retains/retries owned cleanup.
 */
h2_pal_result_t h2_iperf_server_app_request(h2_iperf_server_app_t *app,
                                            h2_iperf_server_app_mode_t mode,
                                            bool running);
/** Thread-safe copy; does not expose worker-owned objects. */
h2_pal_result_t
h2_iperf_server_app_snapshot(h2_iperf_server_app_t *app,
                             h2_iperf_server_app_snapshot_t *out_snapshot);
/** Stop, join workers/manager and release storage. Blocks on the caller task.
 * On join/cleanup error retain the handle so the caller can retry.
 * The manager terminates after one cleanup attempt on shutdown; destroy
 * retries cleanup once after joining it, never looping on a persistent error.
 * The borrowed task joins/network callbacks may themselves block.
 * The caller must quiesce concurrent request/snapshot calls before destroying.
 */
h2_pal_result_t h2_iperf_server_app_destroy(h2_iperf_server_app_t **app);
/** Run the touch UI on the caller task until should_exit is true (NULL means
 * indefinitely). Borrows an existing controller; opens/closes Display/Touch
 * and owns the LVGL singleton for this call. Only this task calls LVGL.
 */
h2_pal_result_t h2_iperf_server_app_run_ui(h2_iperf_server_app_t *app,
                                           bool (*should_exit)(void *user),
                                           void *user);

#ifdef __cplusplus
}
#endif
#endif
