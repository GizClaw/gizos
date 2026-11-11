#ifndef H2_WINDOWS_TLS_TIMEOUT_HOOKS_H
#define H2_WINDOWS_TLS_TIMEOUT_HOOKS_H

/* Include native declarations before substituting dependencies so Winsock's
 * dllimport declarations do not turn test functions into imported symbols. */
#include "../src/h2_windows_internal.h"

h2_pal_result_t h2_test_windows_monotonic_ms(h2_windows_platform_t* platform,
                                             uint64_t* out_ms);
int h2_test_tls_load_ca(WOLFSSL_CTX* context, const unsigned char* data,
                        long len, int format);
int h2_test_tls_load_system_ca(WOLFSSL_CTX* context);
int h2_test_tls_connect(WOLFSSL* ssl);
int h2_test_tls_get_error(WOLFSSL* ssl, int result);
int WSAAPI h2_test_tls_select(int nfds, fd_set* read_fds, fd_set* write_fds,
                              fd_set* error_fds, const struct timeval* timeout);
h2_pal_result_t h2_test_windows_tls_wrap(h2_windows_platform_t* platform,
                                         h2_pal_net_socket_t token,
                                         const h2_pal_net_tls_config_t* config,
                                         uint32_t timeout_ms,
                                         h2_pal_net_socket_t* out_token);

#define h2_windows_monotonic_ms h2_test_windows_monotonic_ms
#define select h2_test_tls_select
#define wolfSSL_CTX_load_verify_buffer h2_test_tls_load_ca
#define wolfSSL_CTX_load_system_CA_certs h2_test_tls_load_system_ca
#define wolfSSL_connect h2_test_tls_connect
#define wolfSSL_get_error h2_test_tls_get_error

/* The real platform supplies socket locking/lifetime. Keep its TLS entry
 * symbols separate from this copy of the provider with controlled timing. */
#define h2_windows_tls_wrap h2_test_windows_tls_wrap
#define h2_windows_tls_send h2_test_windows_tls_send
#define h2_windows_tls_recv h2_test_windows_tls_recv
#define h2_windows_tls_release h2_test_windows_tls_release

#endif
