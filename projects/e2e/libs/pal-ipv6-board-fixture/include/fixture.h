#ifndef H2_IPV6_BOARD_FIXTURE_H
#define H2_IPV6_BOARD_FIXTURE_H
#include "h2/pal/net/h2_pal_dtls.h"
#include "h2_runtime.h"
typedef struct h2_ipv6_tls_evidence {
  int client_hello, certificate_presented, handshake, sni_alpn;
} h2_ipv6_tls_evidence_t;
typedef struct h2_ipv6_tls_server_api {
  void *(*open)(int fd, int expired, h2_ipv6_tls_evidence_t *evidence);
  int (*read)(void *, uint8_t *, size_t);
  int (*write)(void *, const uint8_t *, size_t);
  void (*close)(void *);
} h2_ipv6_tls_server_api_t;
extern const char h2_ipv6_fixture_task_name[];
extern const char h2_ipv6_fixture_worker_task_name[];
extern const char h2_ipv6_fixture_http_task_name[];
extern const char h2_ipv6_fixture_mqtt_task_name[];
extern const char h2_ipv6_fixture_udp_task_name[];
const char *h2_ipv6_fixture_session(void);
const char *h2_ipv6_fixture_certificate(int expired);
const char *h2_ipv6_fixture_key(void);
/* Borrow Runtime/TLS/DTLS for the lifetime of the fixture. Success serves
 * forever. Startup or ready failure requests stop, joins started workers,
 * closes listeners and releases owned protocol/mutex state before returning.
 * A terminal PAL join/destroy failure retains the affected state and borrowed
 * APIs, rejects a new run and returns the cleanup error; the caller must keep
 * those dependencies alive rather than freeing a potentially live context. */
int h2_ipv6_board_fixture_run(h2_runtime_t *, const h2_pal_dtls_api_t *,
                              const h2_ipv6_tls_server_api_t *,
                              int (*ready)(h2_runtime_t *));
#endif
