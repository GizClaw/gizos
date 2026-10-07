#ifndef H2_PAL_NET_TLS_E2E_H
#define H2_PAL_NET_TLS_E2E_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_net_tls_case {
#define H2_NET_TLS_CASE(symbol, id, mandatory) H2_NET_TLS_##symbol,
#include "h2_pal_net_tls_cases.inc"
#undef H2_NET_TLS_CASE
  H2_NET_TLS_CASE_COUNT
} h2_net_tls_case_t;

typedef struct h2_net_tls_case_result {
  const char *id;
  int mandatory, passed, blocked, unsupported, not_assessed, detail;
  unsigned line;
  uint64_t elapsed_ms;
  size_t bytes_sent, bytes_received;
  int provider_result;
  uint8_t observed_ipv4[4];
} h2_net_tls_case_result_t;

typedef struct h2_net_tls_result {
  unsigned passed, failed, blocked, unsupported, not_assessed, mandatory_passed;
  size_t retained_sockets, retained_resolvers, retained_allocations;
  h2_net_tls_case_result_t cases[H2_NET_TLS_CASE_COUNT];
} h2_net_tls_result_t;

typedef enum h2_net_tls_fixture_mode {
  H2_NET_TLS_FIXTURE_TCP,
  H2_NET_TLS_FIXTURE_UDP,
  H2_NET_TLS_FIXTURE_TLS,
  H2_NET_TLS_FIXTURE_EXPIRED,
  H2_NET_TLS_FIXTURE_SILENT,
  H2_NET_TLS_FIXTURE_CLOSE,
  H2_NET_TLS_FIXTURE_CALLBACK,
} h2_net_tls_fixture_mode_t;

typedef enum h2_net_tls_fixture_proof {
  H2_NET_TLS_PROOF_PAYLOAD,
  H2_NET_TLS_PROOF_CERT_REJECTION,
  H2_NET_TLS_PROOF_SNI_ALPN,
  H2_NET_TLS_PROOF_SILENT,
  H2_NET_TLS_PROOF_CLOSED,
  H2_NET_TLS_PROOF_CONNECTED,
} h2_net_tls_fixture_proof_t;

typedef struct h2_net_tls_config {
  const h2_runtime_t *runtime;
  h2_pal_net_family_t family; /* Zero preserves the original IPv4 profile. */
  const char *host; /* Numerical fixture address; not a public service. */
  const char
      *dns_host; /* Explicit real hostname, separate from local TLS peer. */
  h2_pal_net_addr_t dns_expected; /* Independent operator-observed IPv4 A record for this resolver view; both PAL resolutions must equal it. */
  const char *session;            /* 32 lowercase hexadecimal characters. */
  const uint8_t *root_ca, *wrong_ca;
  size_t root_ca_len, wrong_ca_len;
  const char *server_name;
  /* The launcher owns the fixture. Every operation returns within its budget.
   * prepare associates one ephemeral peer/port with this case and session.
   * verify returns OK only with the appropriate current peer evidence. */
  int (*prepare)(void *user, const char *case_id,
                 h2_net_tls_fixture_mode_t mode, uint16_t callback_port,
                 uint16_t *out_port);
  int (*verify)(void *user, const char *case_id,
                h2_net_tls_fixture_proof_t proof);
  void *fixture_user;
  void (*report)(void *user, const h2_net_tls_case_result_t *result);
  void *report_user;
  uint32_t case_timeout_ms;
  uint32_t tls_handshake_timeout_ms;
  int multicast_supported, icmp_supported;
} h2_net_tls_config_t;

/* All 37 mandatory core-profile cases must pass. The two optional capabilities
 * remain explicitly assessed/unsupported, never promoted to full-Net support.
 * App borrows dependencies and fixture, owns only sockets/resolvers/scratch. */
int h2_pal_net_tls_e2e_run(const h2_net_tls_config_t *config,
                           h2_net_tls_result_t *out);

#ifdef __cplusplus
}
#endif
#endif
