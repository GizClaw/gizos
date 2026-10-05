#ifndef H2_IPERF_CLIENT_APP_H
#define H2_IPERF_CLIENT_APP_H

#include "h2_iperf_e2e.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_iperf_client_app_mode {
  H2_IPERF_CLIENT_APP_IPV4 = 4,
  H2_IPERF_CLIENT_APP_IPV6 = 6,
  H2_IPERF_CLIENT_APP_DUAL = 46,
} h2_iperf_client_app_mode_t;

/** Borrowed endpoints and telemetry hook for one blocking run. The launcher
 * owns Wi-Fi and verifies the actual server mode before calling this App.
 * All enabled addresses must have the matching family; hostnames/fallback
 * are deliberately excluded so each measurement has an explicit family.
 */
typedef struct h2_iperf_client_app_config {
  h2_iperf_client_app_mode_t mode;
  /** Optional printable board token; NULL selects "client". */
  const char *target;
  h2_pal_net_addr_t ipv4;
  h2_pal_net_addr_t ipv6;
  /** Zero selects 5201. Overrides the port in either endpoint. */
  uint16_t port;
  /** Zero selects three rounds; maximum ten. */
  unsigned rounds;
  /** Zero selects five seconds per stream. */
  uint32_t duration_ms;
  /** Zero selects 500 ms between cases. */
  uint32_t settle_ms;
  h2_iperf_e2e_checkpoint_fn checkpoint;
  void *checkpoint_user;
} h2_iperf_client_app_config_t;

typedef struct h2_iperf_client_app_report {
  unsigned total;
  unsigned passed;
} h2_iperf_client_app_report_t;

typedef struct h2_iperf_client_app_network {
  h2_iperf_client_app_mode_t mode;
  h2_pal_wifi_sta_status_t wifi;
  uint8_t runtime_ready;
} h2_iperf_client_app_network_t;

/** Temporarily associate using public Wi-Fi PAL and verify the corresponding
 * Runtime IP event/state and Net addresses. Waits 15 seconds after association
 * for DHCP/SLAAC/DAD to settle, within a 45-second budget after association.
 * Drains Runtime events as their single consumer. Link-local alone is
 * insufficient. On success the caller owns the temporary association; on
 * failure after connecting, disconnects best-effort and preserves the
 * original setup error. Clears out_network on failure. */
h2_pal_result_t
h2_iperf_client_app_connect(h2_runtime_t *runtime,
                            const h2_pal_wifi_sta_config_t *config,
                            h2_iperf_client_app_network_t *out_network);

/** Fixed GizOS-iPerf bench: verify public Wi-Fi/Runtime/Net readiness, run
 * the three-round performance matrix, verify disconnect/reconnect and saved
 * credential preservation. Does not persist, initialize SDK networking, or
 * confirm a firmware image. Reports typed readiness and performance receipts.
 * Leaves the verified association active on success; disconnects best-effort
 * on qualification failure and preserves the qualification error.
 * For non-NULL Runtime/target, always observes saved credentials after setup
 * or measurement and attempts exactly one COMPLETE record through Log PAL.
 * The first error survives later lifecycle/saved-state errors. Before family
 * discovery mode is zero; an unstarted matrix has zero cases,
 * matrix_started=0 and matrix_rc=INVALID_STATE. Failure clears all COMPLETE
 * qualification gates; saved_check_rc reports the private comparison result,
 * without exposing credential or signature bytes.
 */
h2_pal_result_t h2_iperf_client_app_bench(h2_runtime_t *runtime,
                                          const char *target,
                                          h2_iperf_e2e_checkpoint_fn checkpoint,
                                          void *checkpoint_user);

/** Run TCP in both directions and UDP at 5/10/20/40 Mbit/s in both
 * directions, using 1200-byte datagrams and 16-KiB TCP blocks. Dual mode
 * runs the same matrix separately over IPv4 and IPv6, never sums throughput.
 * Runtime/PAL views and config remain borrowed until return. No task is
 * created, and no Wi-Fi configuration or Runtime ownership is changed.
 * Every case runs despite earlier failures; OK means all cases exchanged
 * results and received payload, not that a throughput/loss threshold passed.
 * INVALID_ARG clears the report without sending traffic; IO means at least
 * one failed case. Ledger target tokens encode mode, family and round.
 */
h2_pal_result_t
h2_iperf_client_app_run(h2_runtime_t *runtime,
                        const h2_iperf_client_app_config_t *config,
                        h2_iperf_client_app_report_t *out_report);

/** Task declaration consumed by device launchers and the task-policy graph. */
extern const char h2_iperf_client_app_task_name[];

#ifdef __cplusplus
}
#endif
#endif
