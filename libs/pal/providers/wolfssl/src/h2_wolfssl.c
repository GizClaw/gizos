#include "h2_wolfssl_internal.h"

#include "h2/pal/h2_pal_unsupported.h"
#include "h2_wolfcrypt_crypto.h"

#include <string.h>

#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/memory.h>

typedef struct h2_wolfssl_state {
    void *entropy_user;
    h2_wolfssl_entropy_fn entropy;
    wolfSSL_Malloc_cb previous_malloc;
    wolfSSL_Free_cb previous_free;
    wolfSSL_Realloc_cb previous_realloc;
    size_t owner_refs;
    size_t live_sessions;
    int ready;
} h2_wolfssl_state_t;

h2_pal_mem_api_t h2_wolfssl_mem_api;
static h2_wolfssl_state_t h2_wolfssl_state;

int h2_wolfssl_entropy_fill(uint8_t *out, size_t len) {
    if (!h2_wolfssl_state.ready || h2_wolfssl_state.entropy == NULL) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    if (out == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_wolfssl_state.entropy(
        h2_wolfssl_state.entropy_user, out, len);
}

int h2_wolfssl_is_ready(void) {
    return h2_wolfssl_state.ready;
}

h2_pal_result_t h2_wolfssl_session_acquire(void) {
    if (!h2_wolfssl_state.ready) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (h2_wolfssl_state.live_sessions == SIZE_MAX) {
        return H2_PAL_ERR_FULL;
    }
    ++h2_wolfssl_state.live_sessions;
    return H2_PAL_OK;
}

void h2_wolfssl_session_release(void) {
    if (h2_wolfssl_state.live_sessions > 0u) {
        --h2_wolfssl_state.live_sessions;
    }
}

h2_pal_result_t h2_wolfssl_init(const h2_wolfssl_config_t *config) {
    h2_wolfcrypt_crypto_config_t crypto_config;
    int wolf_rc;

    if (config == NULL || config->mem.vtable == NULL ||
        config->mem.vtable->alloc == NULL ||
        config->mem.vtable->realloc == NULL ||
        config->mem.vtable->free == NULL || config->entropy == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (h2_wolfssl_state.ready) {
        if (config->mem.user != h2_wolfssl_mem_api.user ||
            config->mem.vtable != h2_wolfssl_mem_api.vtable ||
            config->entropy_user != h2_wolfssl_state.entropy_user ||
            config->entropy != h2_wolfssl_state.entropy) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        if (h2_wolfssl_state.owner_refs == SIZE_MAX) {
            return H2_PAL_ERR_FULL;
        }
        ++h2_wolfssl_state.owner_refs;
        return H2_PAL_OK;
    }

    memset(&h2_wolfssl_state, 0, sizeof(h2_wolfssl_state));
    h2_wolfssl_mem_api = config->mem;
    h2_wolfssl_state.entropy_user = config->entropy_user;
    h2_wolfssl_state.entropy = config->entropy;
    h2_wolfssl_state.owner_refs = 1u;
    h2_wolfssl_state.ready = 1;

    wolf_rc = wolfSSL_GetAllocators(
        &h2_wolfssl_state.previous_malloc,
        &h2_wolfssl_state.previous_free,
        &h2_wolfssl_state.previous_realloc);
    if (wolf_rc != 0 || wolfSSL_SetAllocators(
            h2_wolfssl_alloc, h2_wolfssl_free,
            h2_wolfssl_realloc) != 0) {
        goto fail;
    }

    crypto_config.entropy_user = config->entropy_user;
    crypto_config.entropy = config->entropy;
    if (h2_wolfcrypt_crypto_init(&crypto_config) != H2_PAL_OK) {
        goto restore_allocators;
    }
    if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
        h2_wolfcrypt_crypto_deinit();
        goto restore_allocators;
    }
    return H2_PAL_OK;

restore_allocators:
    (void)wolfSSL_SetAllocators(
        h2_wolfssl_state.previous_malloc,
        h2_wolfssl_state.previous_free,
        h2_wolfssl_state.previous_realloc);
fail:
    memset(&h2_wolfssl_state, 0, sizeof(h2_wolfssl_state));
    memset(&h2_wolfssl_mem_api, 0, sizeof(h2_wolfssl_mem_api));
    return H2_PAL_ERR_IO;
}

h2_pal_result_t h2_wolfssl_deinit(void) {
    if (!h2_wolfssl_state.ready) {
        return H2_PAL_OK;
    }
    if (h2_wolfssl_state.owner_refs > 1u) {
        --h2_wolfssl_state.owner_refs;
        return H2_PAL_OK;
    }
    if (h2_wolfssl_state.live_sessions != 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    (void)wolfSSL_Cleanup();
    h2_wolfcrypt_crypto_deinit();
    (void)wolfSSL_SetAllocators(
        h2_wolfssl_state.previous_malloc,
        h2_wolfssl_state.previous_free,
        h2_wolfssl_state.previous_realloc);
    memset(&h2_wolfssl_state, 0, sizeof(h2_wolfssl_state));
    memset(&h2_wolfssl_mem_api, 0, sizeof(h2_wolfssl_mem_api));
    return H2_PAL_OK;
}

const h2_pal_crypto_api_t *h2_wolfssl_crypto_api(void) {
    return h2_wolfssl_state.ready
               ? h2_wolfcrypt_crypto_api()
               : h2_pal_unsupported_crypto_api();
}

extern const h2_pal_dtls_api_t h2_wolfssl_dtls_provider;

const h2_pal_dtls_api_t *h2_wolfssl_dtls_api(void) {
    return h2_wolfssl_state.ready
               ? &h2_wolfssl_dtls_provider
               : h2_pal_unsupported_dtls_api();
}
