#include "h2_windows_internal.h"

#include <limits.h>
#include <string.h>

static int windows_tls_io_recv(WOLFSSL *ssl, char *buffer, int size,
                               void *context) {
    (void)ssl;
    h2_windows_socket_slot_t *slot = context;
    int received = recv(slot->socket, buffer, size, 0);
    if (received >= 0) {
        return received;
    }
    int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
        return WOLFSSL_CBIO_ERR_WANT_READ;
    }
    if (error == WSAECONNRESET || error == WSAECONNABORTED ||
        error == WSAENOTCONN) {
        return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    }
    return WOLFSSL_CBIO_ERR_GENERAL;
}

static int windows_tls_io_send(WOLFSSL *ssl, char *buffer, int size,
                               void *context) {
    (void)ssl;
    h2_windows_socket_slot_t *slot = context;
    int sent = send(slot->socket, buffer, size, 0);
    if (sent >= 0) {
        return sent;
    }
    int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
        return WOLFSSL_CBIO_ERR_WANT_WRITE;
    }
    if (error == WSAECONNRESET || error == WSAECONNABORTED ||
        error == WSAENOTCONN) {
        return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    }
    return WOLFSSL_CBIO_ERR_GENERAL;
}

static int windows_tls_wait(SOCKET socket_value, int write,
                            uint32_t timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket_value, &set);
    TIMEVAL timeout;
    timeout.tv_sec = (long)(timeout_ms / 1000u);
    timeout.tv_usec = (long)(timeout_ms % 1000u) * 1000L;
    int selected = select(0, write ? NULL : &set, write ? &set : NULL, NULL,
                          &timeout);
    if (selected > 0) {
        return H2_PAL_OK;
    }
    return selected == 0 ? H2_PAL_ERR_TIMEOUT
                         : h2_windows_error_from_wsa(WSAGetLastError());
}

static uint32_t windows_tls_remaining(h2_windows_platform_t *platform,
                                      uint64_t deadline) {
    uint64_t now = 0u;
    if (h2_windows_monotonic_ms(platform, &now) != H2_PAL_OK ||
        now >= deadline) {
        return 0u;
    }
    uint64_t remaining = deadline - now;
    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static h2_pal_result_t windows_tls_load_ca(
    WOLFSSL_CTX *context, const h2_pal_net_tls_config_t *config) {
    if (config->root_ca_pem == NULL || config->root_ca_pem_len == 0u) {
        return wolfSSL_CTX_load_system_CA_certs(context) == WOLFSSL_SUCCESS
                   ? H2_PAL_OK
                   : H2_PAL_ERR_IO;
    }
    if (config->root_ca_pem_len > INT32_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return wolfSSL_CTX_load_verify_buffer(
               context, config->root_ca_pem,
               (long)config->root_ca_pem_len,
               WOLFSSL_FILETYPE_PEM) == WOLFSSL_SUCCESS
               ? H2_PAL_OK
               : H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t windows_tls_configure_alpn(WOLFSSL *ssl,
                                                   const char *alpn) {
    if (alpn == NULL || alpn[0] == '\0') {
        return H2_PAL_OK;
    }
    size_t len = strlen(alpn);
    if (len > 255u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return wolfSSL_UseALPN(ssl, (char *)alpn, (word32)len,
                           WOLFSSL_ALPN_FAILED_ON_MISMATCH) == WOLFSSL_SUCCESS
               ? H2_PAL_OK
               : H2_PAL_ERR_UNSUPPORTED;
}

static int windows_tls_verify_error(int error) {
    return error == DOMAIN_NAME_MISMATCH || error == IPADDR_MISMATCH ||
           error == VERIFY_CERT_ERROR || error == VERIFY_SIGN_ERROR ||
           error == ASN_NO_SIGNER_E || error == ASN_SELF_SIGNED_E ||
           error == ASN_SIG_CONFIRM_E || error == ASN_BEFORE_DATE_E ||
           error == ASN_AFTER_DATE_E;
}

static h2_pal_result_t windows_tls_handshake(
    h2_windows_platform_t *platform, h2_windows_socket_slot_t *slot,
    uint32_t timeout_ms, h2_pal_net_tls_verify_t verify_mode) {
    uint64_t now = 0u;
    if (h2_windows_monotonic_ms(platform, &now) != H2_PAL_OK ||
        UINT64_MAX - now < timeout_ms) {
        return H2_PAL_ERR_IO;
    }
    uint64_t deadline = now + timeout_ms;
    for (;;) {
        int result = wolfSSL_connect(slot->tls);
        if (result == WOLFSSL_SUCCESS) {
            return H2_PAL_OK;
        }
        int error = wolfSSL_get_error(slot->tls, result);
        if (error != WOLFSSL_ERROR_WANT_READ &&
            error != WOLFSSL_ERROR_WANT_WRITE) {
            return verify_mode == H2_PAL_NET_TLS_VERIFY_REQUIRED &&
                           windows_tls_verify_error(error)
                       ? H2_PAL_ERR_TLS_VERIFY
                       : H2_PAL_ERR_IO;
        }
        uint32_t remaining = windows_tls_remaining(platform, deadline);
        if (remaining == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        h2_pal_result_t wait_result = windows_tls_wait(
            slot->socket, error == WOLFSSL_ERROR_WANT_WRITE, remaining);
        if (wait_result != H2_PAL_OK) {
            return wait_result;
        }
    }
}

h2_pal_result_t h2_windows_tls_wrap(
    h2_windows_platform_t *platform, h2_pal_net_socket_t token,
    const h2_pal_net_tls_config_t *config, uint32_t timeout_ms,
    h2_pal_net_socket_t *out_token) {
    if (platform == NULL || config == NULL || out_token == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_token = -1;
    h2_windows_socket_slot_t *slot = h2_windows_net_lock_slot(platform, token);
    if (slot == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (slot->tls != NULL || slot->connecting) {
        h2_windows_net_unlock_slot(slot);
        return H2_PAL_ERR_INVALID_STATE;
    }
    WOLFSSL_CTX *context = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (context == NULL) {
        h2_windows_net_unlock_slot(slot);
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_pal_result_t result = H2_PAL_OK;
    if (wolfSSL_CTX_SetMinVersion(context, WOLFSSL_TLSV1_2) !=
        WOLFSSL_SUCCESS) {
        result = H2_PAL_ERR_IO;
    }
    h2_pal_net_tls_verify_t verify_mode = config->verify;
    if (verify_mode == H2_PAL_NET_TLS_VERIFY_DEFAULT) {
        verify_mode = H2_PAL_NET_TLS_VERIFY_REQUIRED;
    }
    if (result == H2_PAL_OK &&
        verify_mode == H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY) {
        wolfSSL_CTX_set_verify(context, WOLFSSL_VERIFY_NONE, NULL);
    } else if (result == H2_PAL_OK &&
               verify_mode == H2_PAL_NET_TLS_VERIFY_REQUIRED) {
        if (config->server_name == NULL || config->server_name[0] == '\0') {
            result = H2_PAL_ERR_INVALID_ARG;
        } else {
            result = windows_tls_load_ca(context, config);
            if (result == H2_PAL_OK) {
                wolfSSL_CTX_set_verify(context, WOLFSSL_VERIFY_PEER, NULL);
            }
        }
    } else if (result == H2_PAL_OK) {
        result = H2_PAL_ERR_INVALID_ARG;
    }
    if (result == H2_PAL_OK) {
        wolfSSL_CTX_SetIORecv(context, windows_tls_io_recv);
        wolfSSL_CTX_SetIOSend(context, windows_tls_io_send);
        slot->tls = wolfSSL_new(context);
        if (slot->tls == NULL) {
            result = H2_PAL_ERR_NO_MEMORY;
        }
    }
    if (result == H2_PAL_OK) {
        int groups[] = {WOLFSSL_ECC_X25519, WOLFSSL_ECC_SECP256R1};
        if (wolfSSL_set_groups(slot->tls, groups,
                               (int)(sizeof(groups) / sizeof(groups[0]))) !=
            WOLFSSL_SUCCESS) {
            result = H2_PAL_ERR_IO;
        }
    }
    if (result == H2_PAL_OK && config->server_name != NULL &&
        config->server_name[0] != '\0') {
        size_t name_len = strlen(config->server_name);
        if (name_len > UINT16_MAX ||
            wolfSSL_UseSNI(slot->tls, WOLFSSL_SNI_HOST_NAME,
                           config->server_name,
                           (unsigned short)name_len) != WOLFSSL_SUCCESS ||
            wolfSSL_check_domain_name(slot->tls, config->server_name) !=
                WOLFSSL_SUCCESS) {
            result = H2_PAL_ERR_IO;
        }
    }
    if (result == H2_PAL_OK) {
        result = windows_tls_configure_alpn(slot->tls, config->alpn);
    }
    if (result == H2_PAL_OK) {
        wolfSSL_SetIOReadCtx(slot->tls, slot);
        wolfSSL_SetIOWriteCtx(slot->tls, slot);
        slot->tls_context = context;
        result = windows_tls_handshake(platform, slot, timeout_ms,
                                       verify_mode);
    }
    if (result != H2_PAL_OK) {
        if (slot->tls != NULL) {
            wolfSSL_free(slot->tls);
            slot->tls = NULL;
        }
        wolfSSL_CTX_free(context);
        slot->tls_context = NULL;
        h2_windows_net_unlock_slot(slot);
        return result;
    }
    *out_token = token;
    h2_windows_net_unlock_slot(slot);
    return H2_PAL_OK;
}

static int windows_tls_io_result(WOLFSSL *ssl, int result) {
    if (result > 0) {
        return result;
    }
    int error = wolfSSL_get_error(ssl, result);
    if (error == WOLFSSL_ERROR_WANT_READ ||
        error == WOLFSSL_ERROR_WANT_WRITE) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    return error == WOLFSSL_ERROR_ZERO_RETURN ? H2_PAL_ERR_CLOSED
                                               : H2_PAL_ERR_IO;
}

int h2_windows_tls_send(h2_windows_platform_t *platform,
                        h2_windows_socket_slot_t *slot, const uint8_t *data,
                        size_t len, uint32_t timeout_ms) {
    if (len > INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint64_t now = 0u;
    if (h2_windows_monotonic_ms(platform, &now) != H2_PAL_OK ||
        UINT64_MAX - now < timeout_ms) {
        return H2_PAL_ERR_IO;
    }
    uint64_t deadline = now + timeout_ms;
    for (;;) {
        int written = wolfSSL_write(slot->tls, data, (int)len);
        int result = windows_tls_io_result(slot->tls, written);
        if (result != H2_PAL_ERR_WOULD_BLOCK) {
            return result;
        }
        uint32_t remaining = windows_tls_remaining(platform, deadline);
        if (remaining == 0u) {
            return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                    : H2_PAL_ERR_TIMEOUT;
        }
        int error = wolfSSL_get_error(slot->tls, written);
        result = windows_tls_wait(slot->socket,
                                  error == WOLFSSL_ERROR_WANT_WRITE,
                                  remaining);
        if (result != H2_PAL_OK) {
            return result;
        }
    }
}

int h2_windows_tls_recv(h2_windows_platform_t *platform,
                        h2_windows_socket_slot_t *slot, uint8_t *data,
                        size_t len, uint32_t timeout_ms) {
    if (len > INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint64_t now = 0u;
    if (h2_windows_monotonic_ms(platform, &now) != H2_PAL_OK ||
        UINT64_MAX - now < timeout_ms) {
        return H2_PAL_ERR_IO;
    }
    uint64_t deadline = now + timeout_ms;
    for (;;) {
        int received = wolfSSL_read(slot->tls, data, (int)len);
        int result = windows_tls_io_result(slot->tls, received);
        if (result != H2_PAL_ERR_WOULD_BLOCK) {
            return result;
        }
        uint32_t remaining = windows_tls_remaining(platform, deadline);
        if (remaining == 0u) {
            return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                    : H2_PAL_ERR_TIMEOUT;
        }
        int error = wolfSSL_get_error(slot->tls, received);
        result = windows_tls_wait(slot->socket,
                                  error == WOLFSSL_ERROR_WANT_WRITE,
                                  remaining);
        if (result != H2_PAL_OK) {
            return result;
        }
    }
}

void h2_windows_tls_release(h2_windows_socket_slot_t *slot) {
    if (slot->tls != NULL) {
        wolfSSL_free(slot->tls);
        slot->tls = NULL;
    }
    if (slot->tls_context != NULL) {
        wolfSSL_CTX_free(slot->tls_context);
        slot->tls_context = NULL;
    }
}
