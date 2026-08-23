#include "h2_pal_host_local_peers.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <wolfssl/ssl.h>

typedef enum peer_kind {
    PEER_TCP_ECHO,
    PEER_TLS_ECHO,
    PEER_TLS_REJECT,
    PEER_HTTPS,
    PEER_MQTT,
} peer_kind_t;

typedef struct local_peer {
    struct h2_pal_host_local_peers *owner;
    HANDLE thread;
    SOCKET listen_socket;
    SOCKET accepted_socket;
    CRITICAL_SECTION lock;
    LONG status;
    peer_kind_t kind;
    uint16_t port;
} local_peer_t;

struct h2_pal_host_local_peers {
    local_peer_t peers[5];
    uint8_t *root_ca;
    size_t root_ca_len;
    uint8_t *wrong_ca;
    size_t wrong_ca_len;
    const char *certificate_path;
    const char *private_key_path;
};

static int read_exact(SOCKET socket_value, uint8_t *data, size_t len) {
    size_t offset = 0u;
    while (offset < len) {
        int count = recv(socket_value, (char *)data + offset,
                         (int)(len - offset), 0);
        if (count <= 0) return -1;
        offset += (size_t)count;
    }
    return 0;
}

static int write_exact(SOCKET socket_value, const uint8_t *data, size_t len) {
    size_t offset = 0u;
    while (offset < len) {
        int count = send(socket_value, (const char *)data + offset,
                         (int)(len - offset), 0);
        if (count <= 0) return -1;
        offset += (size_t)count;
    }
    return 0;
}

static size_t mqtt_header_len(const uint8_t *packet, size_t packet_len) {
    size_t header_len = 1u;
    while (header_len < packet_len && header_len < 5u) {
        if ((packet[header_len++] & 0x80u) == 0u) return header_len;
    }
    return 0u;
}

static int read_mqtt_packet(SOCKET socket_value, uint8_t *packet,
                            size_t capacity, size_t *out_len) {
    if (capacity < 2u || read_exact(socket_value, packet, 1u) != 0) return -1;
    size_t header_len = 1u;
    size_t remaining = 0u;
    size_t multiplier = 1u;
    uint8_t encoded = 0u;
    do {
        if (header_len >= 5u ||
            read_exact(socket_value, &encoded, 1u) != 0) return -1;
        packet[header_len++] = encoded;
        remaining += (size_t)(encoded & 0x7fu) * multiplier;
        multiplier *= 128u;
    } while ((encoded & 0x80u) != 0u);
    if (remaining > capacity - header_len ||
        read_exact(socket_value, packet + header_len, remaining) != 0) {
        return -1;
    }
    *out_len = header_len + remaining;
    return (int)(packet[0] >> 4u);
}

static int serve_mqtt(SOCKET socket_value) {
    uint8_t packet[4096];
    for (;;) {
        size_t packet_len = 0u;
        int type = read_mqtt_packet(socket_value, packet, sizeof(packet),
                                    &packet_len);
        if (type == 1) {
            static const uint8_t response[] = {0x20u, 0x02u, 0x00u, 0x00u};
            if (write_exact(socket_value, response, sizeof(response)) != 0)
                return -1;
        } else if (type == 8) {
            size_t header_len = mqtt_header_len(packet, packet_len);
            if (header_len == 0u || packet_len - header_len < 2u) return -1;
            uint8_t response[] = {0x90u, 0x03u, packet[header_len],
                                  packet[header_len + 1u], 0x00u};
            if (write_exact(socket_value, response, sizeof(response)) != 0)
                return -1;
        } else if (type == 3) {
            if (write_exact(socket_value, packet, packet_len) != 0) return -1;
        } else if (type == 14) {
            return 0;
        } else {
            return -1;
        }
    }
}

static int tls_recv(WOLFSSL *ssl, char *buffer, int size, void *context) {
    (void)ssl;
    local_peer_t *peer = context;
    int result = recv(peer->accepted_socket, buffer, size, 0);
    if (result >= 0) return result;
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK ? WOLFSSL_CBIO_ERR_WANT_READ
                                   : WOLFSSL_CBIO_ERR_GENERAL;
}

static int tls_send(WOLFSSL *ssl, char *buffer, int size, void *context) {
    (void)ssl;
    local_peer_t *peer = context;
    int result = send(peer->accepted_socket, buffer, size, 0);
    if (result >= 0) return result;
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK ? WOLFSSL_CBIO_ERR_WANT_WRITE
                                   : WOLFSSL_CBIO_ERR_GENERAL;
}

static int serve_tls(local_peer_t *peer) {
    WOLFSSL_CTX *context = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (context == NULL ||
        wolfSSL_CTX_use_certificate_file(context,
            peer->owner->certificate_path, WOLFSSL_FILETYPE_PEM) !=
            WOLFSSL_SUCCESS ||
        wolfSSL_CTX_use_PrivateKey_file(context,
            peer->owner->private_key_path, WOLFSSL_FILETYPE_PEM) !=
            WOLFSSL_SUCCESS) {
        wolfSSL_CTX_free(context);
        return -1;
    }
    wolfSSL_CTX_SetIORecv(context, tls_recv);
    wolfSSL_CTX_SetIOSend(context, tls_send);
    WOLFSSL *ssl = wolfSSL_new(context);
    if (ssl == NULL) {
        wolfSSL_CTX_free(context);
        return -1;
    }
    wolfSSL_SetIOReadCtx(ssl, peer);
    wolfSSL_SetIOWriteCtx(ssl, peer);
    int accepted = wolfSSL_accept(ssl);
    int status = -1;
    if (peer->kind == PEER_TLS_REJECT) {
        status = accepted == WOLFSSL_SUCCESS ? -1 : 0;
    } else if (accepted == WOLFSSL_SUCCESS && peer->kind == PEER_TLS_ECHO) {
        uint8_t request[5];
        static const uint8_t response[] = "ok";
        status = wolfSSL_read(ssl, request, sizeof(request)) ==
                         (int)sizeof(request) &&
                         memcmp(request, "ping", sizeof(request)) == 0 &&
                         wolfSSL_write(ssl, response, sizeof(response)) ==
                             (int)sizeof(response)
                     ? 0 : -1;
    } else if (accepted == WOLFSSL_SUCCESS && peer->kind == PEER_HTTPS) {
        char request[1024] = {0};
        size_t used = 0u;
        while (used + 1u < sizeof(request) &&
               strstr(request, "\r\n\r\n") == NULL) {
            int count = wolfSSL_read(ssl, request + used,
                                     (int)(sizeof(request) - used - 1u));
            if (count <= 0) break;
            used += (size_t)count;
            request[used] = '\0';
        }
        static const char response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n"
            "Connection: close\r\n\r\nhost-e2e";
        status = strstr(request, "GET /pal-host-e2e HTTP/1.1\r\n") == request &&
                         wolfSSL_write(ssl, response,
                                       (int)(sizeof(response) - 1u)) ==
                             (int)(sizeof(response) - 1u)
                     ? 0 : -1;
    }
    if (accepted == WOLFSSL_SUCCESS) (void)wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    wolfSSL_CTX_free(context);
    return status;
}

static DWORD WINAPI peer_thread(void *user) {
    local_peer_t *peer = user;
    peer->status = H2_PAL_ERR_IO;
    EnterCriticalSection(&peer->lock);
    SOCKET listener_for_accept = peer->listen_socket;
    LeaveCriticalSection(&peer->lock);
    SOCKET accepted = accept(listener_for_accept, NULL, NULL);
    if (accepted != INVALID_SOCKET) {
        DWORD io_timeout_ms = 3000u;
        (void)setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO,
                         (const char *)&io_timeout_ms,
                         (int)sizeof(io_timeout_ms));
        (void)setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO,
                         (const char *)&io_timeout_ms,
                         (int)sizeof(io_timeout_ms));
        EnterCriticalSection(&peer->lock);
        peer->accepted_socket = accepted;
        LeaveCriticalSection(&peer->lock);
        int status = -1;
        if (peer->kind == PEER_TCP_ECHO) {
            uint8_t payload[11];
            status = read_exact(peer->accepted_socket, payload,
                                sizeof(payload)) == 0 &&
                             write_exact(peer->accepted_socket, payload,
                                         sizeof(payload)) == 0
                         ? 0 : -1;
        } else if (peer->kind == PEER_MQTT) {
            status = serve_mqtt(peer->accepted_socket);
        } else {
            status = serve_tls(peer);
        }
        if (status == 0) peer->status = H2_PAL_OK;
        EnterCriticalSection(&peer->lock);
        if (peer->accepted_socket == accepted) {
            peer->accepted_socket = INVALID_SOCKET;
        }
        LeaveCriticalSection(&peer->lock);
        (void)closesocket(accepted);
    }
    EnterCriticalSection(&peer->lock);
    SOCKET listener = peer->listen_socket;
    peer->listen_socket = INVALID_SOCKET;
    LeaveCriticalSection(&peer->lock);
    if (listener != INVALID_SOCKET) (void)closesocket(listener);
    return 0u;
}

static int localhost_family(void) {
    ADDRINFOEXW hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOEXW *addresses = NULL;
    int result = GetAddrInfoExW(L"localhost", NULL, NS_ALL, NULL, &hints,
                                &addresses, NULL, NULL, NULL, NULL);
    int family = AF_UNSPEC;
    if (result == 0) {
        for (ADDRINFOEXW *entry = addresses; entry != NULL;
             entry = entry->ai_next) {
            if (entry->ai_family == AF_INET || entry->ai_family == AF_INET6) {
                family = entry->ai_family;
                break;
            }
        }
    }
    if (addresses != NULL) FreeAddrInfoExW(addresses);
    return family;
}

static h2_pal_result_t start_peer(h2_pal_host_local_peers_t *owner,
                                  size_t index, peer_kind_t kind) {
    local_peer_t *peer = &owner->peers[index];
    peer->owner = owner;
    peer->kind = kind;
    const int family = kind == PEER_HTTPS ? localhost_family() : AF_INET;
    if (family == AF_UNSPEC) return H2_PAL_ERR_NOT_FOUND;
    peer->listen_socket = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (peer->listen_socket == INVALID_SOCKET) return H2_PAL_ERR_IO;
    SOCKADDR_STORAGE storage;
    memset(&storage, 0, sizeof(storage));
    int address_len = 0;
    if (family == AF_INET6) {
        SOCKADDR_IN6 *address = (SOCKADDR_IN6 *)&storage;
        address->sin6_family = AF_INET6;
        address->sin6_addr = in6addr_loopback;
        address_len = (int)sizeof(*address);
    } else {
        SOCKADDR_IN *address = (SOCKADDR_IN *)&storage;
        address->sin_family = AF_INET;
        address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_len = (int)sizeof(*address);
    }
    if (bind(peer->listen_socket, (SOCKADDR *)&storage, address_len) != 0 ||
        listen(peer->listen_socket, 1) != 0 ||
        getsockname(peer->listen_socket, (SOCKADDR *)&storage,
                    &address_len) != 0) {
        (void)closesocket(peer->listen_socket);
        peer->listen_socket = INVALID_SOCKET;
        return H2_PAL_ERR_IO;
    }
    peer->thread = CreateThread(NULL, 0u, peer_thread, peer, 0u, NULL);
    if (peer->thread == NULL) {
        (void)closesocket(peer->listen_socket);
        peer->listen_socket = INVALID_SOCKET;
        return H2_PAL_ERR_NO_MEMORY;
    }
    peer->port = family == AF_INET6
                     ? ntohs(((SOCKADDR_IN6 *)&storage)->sin6_port)
                     : ntohs(((SOCKADDR_IN *)&storage)->sin_port);
    return H2_PAL_OK;
}

static h2_pal_result_t read_file(const char *path, uint8_t **out_data,
                                 size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return H2_PAL_ERR_IO;
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return H2_PAL_ERR_IO;
    }
    long len = ftell(file);
    if (len <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return H2_PAL_ERR_IO;
    }
    uint8_t *data = malloc((size_t)len);
    if (data == NULL || fread(data, 1u, (size_t)len, file) != (size_t)len) {
        free(data);
        (void)fclose(file);
        return H2_PAL_ERR_IO;
    }
    (void)fclose(file);
    *out_data = data;
    *out_len = (size_t)len;
    return H2_PAL_OK;
}

h2_pal_result_t h2_pal_host_local_peers_create(
    const h2_pal_host_fixture_config_t *config,
    h2_pal_host_local_peers_t **out_peers,
    h2_pal_host_local_peer_endpoints_t *out_endpoints) {
    if (config == NULL || config->root_ca_path == NULL ||
        config->wrong_ca_path == NULL || config->certificate_path == NULL ||
        config->private_key_path == NULL || out_peers == NULL ||
        out_endpoints == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_peers = NULL;
    memset(out_endpoints, 0, sizeof(*out_endpoints));
    h2_pal_host_local_peers_t *peers = calloc(1u, sizeof(*peers));
    if (peers == NULL) return H2_PAL_ERR_NO_MEMORY;
    for (size_t index = 0u; index < 5u; ++index) {
        InitializeCriticalSection(&peers->peers[index].lock);
        peers->peers[index].listen_socket = INVALID_SOCKET;
        peers->peers[index].accepted_socket = INVALID_SOCKET;
    }
    peers->certificate_path = config->certificate_path;
    peers->private_key_path = config->private_key_path;
    h2_pal_result_t result = read_file(config->root_ca_path, &peers->root_ca,
                                       &peers->root_ca_len);
    if (result == H2_PAL_OK) {
        result = read_file(config->wrong_ca_path, &peers->wrong_ca,
                           &peers->wrong_ca_len);
    }
    const peer_kind_t kinds[] = {PEER_TCP_ECHO, PEER_TLS_ECHO,
                                 PEER_TLS_REJECT, PEER_HTTPS, PEER_MQTT};
    for (size_t index = 0u; result == H2_PAL_OK && index < 5u; ++index) {
        result = start_peer(peers, index, kinds[index]);
    }
    if (result != H2_PAL_OK) {
        (void)h2_pal_host_local_peers_destroy(peers);
        return result;
    }
    out_endpoints->tcp_echo_port = peers->peers[0].port;
    out_endpoints->tls_echo_port = peers->peers[1].port;
    out_endpoints->tls_wrong_ca_port = peers->peers[2].port;
    out_endpoints->https_port = peers->peers[3].port;
    out_endpoints->mqtt_port = peers->peers[4].port;
    out_endpoints->root_ca_pem = peers->root_ca;
    out_endpoints->root_ca_pem_len = peers->root_ca_len;
    out_endpoints->wrong_ca_pem = peers->wrong_ca;
    out_endpoints->wrong_ca_pem_len = peers->wrong_ca_len;
    *out_peers = peers;
    return H2_PAL_OK;
}

h2_pal_result_t h2_pal_host_local_peers_destroy(
    h2_pal_host_local_peers_t *peers) {
    if (peers == NULL) return H2_PAL_OK;
    h2_pal_result_t result = H2_PAL_OK;
    for (size_t index = 0u; index < 5u; ++index) {
        local_peer_t *peer = &peers->peers[index];
        EnterCriticalSection(&peer->lock);
        SOCKET listener = peer->listen_socket;
        peer->listen_socket = INVALID_SOCKET;
        SOCKET accepted = peer->accepted_socket;
        LeaveCriticalSection(&peer->lock);
        if (listener != INVALID_SOCKET) {
            (void)closesocket(listener);
        }
        if (accepted != INVALID_SOCKET) {
            (void)shutdown(accepted, SD_BOTH);
        }
    }
    int all_done = 1;
    for (size_t index = 0u; index < 5u; ++index) {
        local_peer_t *peer = &peers->peers[index];
        if (peer->thread != NULL) {
            if (WaitForSingleObject(peer->thread, 7000u) != WAIT_OBJECT_0) {
                all_done = 0;
                result = H2_PAL_ERR_IO;
            } else {
                if (!CloseHandle(peer->thread) ||
                    peer->status != H2_PAL_OK) {
                    result = H2_PAL_ERR_IO;
                }
                peer->thread = NULL;
            }
        }
    }
    if (!all_done) return H2_PAL_ERR_BUSY;
    for (size_t index = 0u; index < 5u; ++index) {
        DeleteCriticalSection(&peers->peers[index].lock);
    }
    free(peers->root_ca);
    free(peers->wrong_ca);
    free(peers);
    return result;
}
