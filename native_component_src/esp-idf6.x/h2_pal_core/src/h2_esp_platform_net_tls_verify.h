#ifndef H2_ESP_PLATFORM_NET_TLS_VERIFY_H
#define H2_ESP_PLATFORM_NET_TLS_VERIFY_H

#include "mbedtls/x509_crt.h"
#include <stdbool.h>

/* Borrowed SDK callback and context installed before PAL adds its calibrated
 * date checks. The TLS socket owns this wrapper through handshake/teardown. */
typedef struct h2_esp_net_tls_verify {
    int (*delegate)(void *user, mbedtls_x509_crt *cert, int depth,
                    uint32_t *flags);
    void *delegate_user;
    bool certificate_bundle;
} h2_esp_net_tls_verify_t;

/* NULL now means PAL wall time is unavailable and fails verified TLS. SDK
 * flags/fatal results remain authoritative; real peer dates are always checked. */
int h2_esp_net_tls_verify_certificate(const h2_esp_net_tls_verify_t *verify,
                                    mbedtls_x509_crt *cert, int depth,
                                    uint32_t *flags,
                                    const mbedtls_x509_time *now);

#endif
