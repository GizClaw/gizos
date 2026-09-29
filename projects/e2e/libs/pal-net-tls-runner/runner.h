#ifndef H2_NET_TLS_RUNNER_H
#define H2_NET_TLS_RUNNER_H
#include "h2_pal_net_tls_e2e.h"
#include <stdio.h>
typedef struct h2_net_tls_fixture_client {
  const h2_runtime_t *runtime;
  const char *host, *session;
  uint16_t port;
  const char *run_id; /* 16 hex chars, default session prefix for one-process
                         fixtures */
} h2_net_tls_fixture_client_t;
int h2_net_tls_fixture_prepare(void *, const char *, h2_net_tls_fixture_mode_t,
                               uint16_t, uint16_t *);
int h2_net_tls_fixture_verify(void *, const char *, h2_net_tls_fixture_proof_t);
void h2_net_tls_report(void *, const h2_net_tls_case_result_t *);
void h2_net_tls_summary(FILE *, const h2_net_tls_result_t *, int, int);
int h2_net_tls_write_report(const char *, const char *,
                            const h2_net_tls_result_t *, int, int);
#endif
