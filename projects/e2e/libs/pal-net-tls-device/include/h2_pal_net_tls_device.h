#ifndef H2_NET_TLS_DEVICE_H
#define H2_NET_TLS_DEVICE_H
#include "runner.h"
extern const char h2_pal_net_tls_device_runner_task_name[];
int h2_net_tls_device_prepare_network(h2_runtime_t *runtime);
int h2_pal_net_tls_device_run(h2_runtime_t *runtime,
                              h2_net_tls_result_t *result);
void h2_pal_net_tls_device_report(const h2_runtime_t *runtime,
                                  const h2_net_tls_result_t *result);
#endif
