#ifndef H2_NET_TLS_DEVICE_H
#define H2_NET_TLS_DEVICE_H
#include "runner.h"
extern const char h2_pal_net_tls_device_runner_task_name[];
int h2_net_tls_device_prepare_network(h2_runtime_t *runtime);
/** Borrow optional fixture credentials until return; NULL retains the original
 * GOT_IP/saved setup. Uses non-saving connect with the existing setup budgets. */
int h2_net_tls_device_prepare_network_with_wifi(h2_runtime_t *runtime,
    const h2_pal_wifi_sta_config_t *wifi);
int h2_pal_net_tls_device_run(h2_runtime_t *runtime,
                              h2_net_tls_result_t *result);
/** Run the original suite after network setup with borrowed optional fixture
 * credentials. NULL retains saved-network setup; no settings are persisted. */
int h2_pal_net_tls_device_run_with_wifi(h2_runtime_t *runtime,
    const h2_pal_wifi_sta_config_t *wifi, h2_net_tls_result_t *result);
void h2_pal_net_tls_device_report(const h2_runtime_t *runtime,
                                  const h2_net_tls_result_t *result);
#endif
