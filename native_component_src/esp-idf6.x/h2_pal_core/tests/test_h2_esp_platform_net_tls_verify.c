#include "h2_esp_platform_net_tls_verify.h"
#include <assert.h>
#include <stdio.h>

typedef struct sdk_verify {
    unsigned calls;
    bool trusted_anchor;
    int result;
} sdk_verify_t;

int mbedtls_x509_time_cmp(const mbedtls_x509_time *a,
                          const mbedtls_x509_time *b) {
    const int aa[] = {a->year, a->mon, a->day, a->hour, a->min, a->sec};
    const int bb[] = {b->year, b->mon, b->day, b->hour, b->min, b->sec};
    for (size_t i = 0u; i < 6u; ++i) {
        if (aa[i] != bb[i]) return aa[i] < bb[i] ? -1 : 1;
    }
    return 0;
}

static int sdk_delegate(void *user, mbedtls_x509_crt *cert, int depth,
                         uint32_t *flags) {
    sdk_verify_t *sdk = user;
    ++sdk->calls;
    if (sdk->result != 0) return sdk->result;
    /* This is the SDK's bundle-membership decision, never PAL's decision. */
    if (cert->raw.p == NULL && depth > 0 && sdk->trusted_anchor)
        *flags &= ~(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    else if (cert->raw.p == NULL && depth > 0)
        *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    return 0;
}

int main(void) {
    const mbedtls_x509_time now = {2026, 10, 3, 0, 0, 0};
    sdk_verify_t sdk = {.trusted_anchor = true};
    h2_esp_net_tls_verify_t verify = {
        .delegate = sdk_delegate, .delegate_user = &sdk,
        .certificate_bundle = true,
    };
    mbedtls_x509_crt anchor = {0};
    uint32_t flags = MBEDTLS_X509_BADCERT_EXPIRED;
    assert(h2_esp_net_tls_verify_certificate(&verify, &anchor, 2, &flags, &now) == 0);
    assert(flags == 0u && sdk.calls == 1u);

    sdk.trusted_anchor = false;
    flags = MBEDTLS_X509_BADCERT_EXPIRED;
    assert(h2_esp_net_tls_verify_certificate(&verify, &anchor, 2, &flags, &now) == 0);
    assert((flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0u);
    assert((flags & MBEDTLS_X509_BADCERT_EXPIRED) != 0u);

    unsigned char der = 0x30;
    mbedtls_x509_crt leaf = {
        .raw = {.p = &der, .len = 1u},
        .valid_from = {2026, 9, 16, 0, 0, 0},
        .valid_to = {2026, 12, 15, 0, 0, 0},
    };
    sdk.trusted_anchor = true;
    flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    assert(h2_esp_net_tls_verify_certificate(&verify, &leaf, 0, &flags, &now) == 0);
    assert(flags == MBEDTLS_X509_BADCERT_NOT_TRUSTED);
    flags = 0u;
    leaf.valid_to.year = 2025;
    assert(h2_esp_net_tls_verify_certificate(&verify, &leaf, 0, &flags, &now) == 0);
    assert(flags == MBEDTLS_X509_BADCERT_EXPIRED);
    leaf.valid_to.year = 2030;
    leaf.valid_from.year = 2027;
    flags = 0u;
    assert(h2_esp_net_tls_verify_certificate(&verify, &leaf, 0, &flags, &now) == 0);
    assert(flags == MBEDTLS_X509_BADCERT_FUTURE);

    /* A zero-date leaf or custom-PEM root cannot borrow the bundle exception. */
    leaf.valid_from = (mbedtls_x509_time){0};
    leaf.valid_to = (mbedtls_x509_time){0};
    flags = 0u;
    assert(h2_esp_net_tls_verify_certificate(&verify, &leaf, 0, &flags, &now) == 0);
    assert(flags == MBEDTLS_X509_BADCERT_EXPIRED);
    verify.certificate_bundle = false;
    flags = 0u;
    assert(h2_esp_net_tls_verify_certificate(&verify, &anchor, 2, &flags, &now) == 0);
    assert(flags == MBEDTLS_X509_BADCERT_EXPIRED);
    verify.certificate_bundle = true;
    verify.delegate = NULL;
    flags = 0u;
    assert(h2_esp_net_tls_verify_certificate(&verify, &anchor, 2, &flags, &now) == 0);
    assert(flags == MBEDTLS_X509_BADCERT_EXPIRED);

    verify.delegate = sdk_delegate;
    flags = 0u;
    const unsigned before = sdk.calls;
    assert(h2_esp_net_tls_verify_certificate(&verify, &anchor, 2, &flags, NULL) == 0);
    assert(flags == MBEDTLS_X509_BADCERT_OTHER && sdk.calls == before + 1u);
    sdk.result = -42;
    flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    assert(h2_esp_net_tls_verify_certificate(&verify, &leaf, 0, &flags, &now) == -42);
    assert(flags == MBEDTLS_X509_BADCERT_NOT_TRUSTED);
    puts("ESP_TLS_VERIFY delegate=preserved anchor=accepted expired=future=untrusted=rejected clock=required");
    return 0;
}
