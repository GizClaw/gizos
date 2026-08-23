#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "h2_pal_host_local_peers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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
    pthread_t thread;
    int listen_fd;
    int accepted_fd;
    pthread_mutex_t lock;
    int started;
    int status;
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
    void (*previous_sigpipe)(int);
    int sigpipe_changed;
};

static int read_exact(int fd, uint8_t *data, size_t len) {
    size_t offset = 0u;
    while (offset < len) {
        ssize_t count = recv(fd, data + offset, len - offset, 0);
        if (count > 0) offset += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static int write_exact(int fd, const uint8_t *data, size_t len) {
    size_t offset = 0u;
    while (offset < len) {
        ssize_t count = send(fd, data + offset, len - offset, 0);
        if (count > 0) offset += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else return -1;
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

static int read_mqtt_packet(int fd, uint8_t *packet, size_t capacity,
                            size_t *out_len) {
    if (capacity < 2u || read_exact(fd, packet, 1u) != 0) return -1;
    size_t header_len = 1u;
    size_t remaining = 0u;
    size_t multiplier = 1u;
    uint8_t encoded = 0u;
    do {
        if (header_len >= 5u || read_exact(fd, &encoded, 1u) != 0) return -1;
        packet[header_len++] = encoded;
        remaining += (size_t)(encoded & 0x7fu) * multiplier;
        multiplier *= 128u;
    } while ((encoded & 0x80u) != 0u);
    if (remaining > capacity - header_len ||
        read_exact(fd, packet + header_len, remaining) != 0) return -1;
    *out_len = header_len + remaining;
    return (int)(packet[0] >> 4u);
}

static int serve_mqtt(int fd) {
    uint8_t packet[4096];
    for (;;) {
        size_t packet_len = 0u;
        int type = read_mqtt_packet(fd, packet, sizeof(packet), &packet_len);
        if (type == 1) {
            static const uint8_t response[] = {0x20u, 0x02u, 0x00u, 0x00u};
            if (write_exact(fd, response, sizeof(response)) != 0) return -1;
        } else if (type == 8) {
            size_t header_len = mqtt_header_len(packet, packet_len);
            if (header_len == 0u || packet_len - header_len < 2u) return -1;
            uint8_t response[] = {0x90u, 0x03u, packet[header_len],
                                  packet[header_len + 1u], 0x00u};
            if (write_exact(fd, response, sizeof(response)) != 0) return -1;
        } else if (type == 3) {
            if (write_exact(fd, packet, packet_len) != 0) return -1;
        } else if (type == 14) {
            return 0;
        } else {
            return -1;
        }
    }
}

static int serve_tls(local_peer_t *peer, int fd) {
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
    WOLFSSL *ssl = wolfSSL_new(context);
    if (ssl == NULL || wolfSSL_set_fd(ssl, fd) != WOLFSSL_SUCCESS) {
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(context);
        return -1;
    }
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

static void *peer_thread(void *user) {
    local_peer_t *peer = user;
    sigset_t blocked;
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &blocked, NULL);
    peer->status = H2_PAL_ERR_IO;
    (void)pthread_mutex_lock(&peer->lock);
    int listener_for_accept = peer->listen_fd;
    (void)pthread_mutex_unlock(&peer->lock);
    int fd = accept(listener_for_accept, NULL, NULL);
    if (fd >= 0) {
        struct timeval io_timeout = {.tv_sec = 3, .tv_usec = 0};
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout,
                         sizeof(io_timeout));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout,
                         sizeof(io_timeout));
        (void)pthread_mutex_lock(&peer->lock);
        peer->accepted_fd = fd;
        (void)pthread_mutex_unlock(&peer->lock);
        int status = -1;
        if (peer->kind == PEER_TCP_ECHO) {
            uint8_t payload[11];
            status = read_exact(fd, payload, sizeof(payload)) == 0 &&
                             write_exact(fd, payload, sizeof(payload)) == 0
                         ? 0 : -1;
        } else if (peer->kind == PEER_MQTT) {
            status = serve_mqtt(fd);
        } else {
            status = serve_tls(peer, fd);
        }
        if (status == 0) peer->status = H2_PAL_OK;
        (void)pthread_mutex_lock(&peer->lock);
        if (peer->accepted_fd == fd) peer->accepted_fd = -1;
        (void)pthread_mutex_unlock(&peer->lock);
        (void)close(fd);
    }
    (void)pthread_mutex_lock(&peer->lock);
    int listener = peer->listen_fd;
    peer->listen_fd = -1;
    (void)pthread_mutex_unlock(&peer->lock);
    if (listener >= 0) (void)close(listener);
    return NULL;
}

static h2_pal_result_t start_peer(h2_pal_host_local_peers_t *owner,
                                  size_t index, peer_kind_t kind) {
    local_peer_t *peer = &owner->peers[index];
    peer->owner = owner;
    peer->kind = kind;
    peer->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (peer->listen_fd < 0) return H2_PAL_ERR_IO;
    int reuse = 1;
    (void)setsockopt(peer->listen_fd, SOL_SOCKET, SO_REUSEADDR,
                     &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(peer->listen_fd, (struct sockaddr *)&address,
             sizeof(address)) != 0 || listen(peer->listen_fd, 1) != 0) {
        (void)close(peer->listen_fd);
        peer->listen_fd = -1;
        return H2_PAL_ERR_IO;
    }
    socklen_t address_len = sizeof(address);
    if (getsockname(peer->listen_fd, (struct sockaddr *)&address,
                    &address_len) != 0 ||
        pthread_create(&peer->thread, NULL, peer_thread, peer) != 0) {
        (void)close(peer->listen_fd);
        peer->listen_fd = -1;
        return H2_PAL_ERR_IO;
    }
    peer->started = 1;
    peer->port = ntohs(address.sin_port);
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
    peers->previous_sigpipe = signal(SIGPIPE, SIG_IGN);
    if (peers->previous_sigpipe == SIG_ERR) {
        free(peers);
        return H2_PAL_ERR_IO;
    }
    peers->sigpipe_changed = 1;
    for (size_t index = 0u; index < 5u; ++index) {
        if (pthread_mutex_init(&peers->peers[index].lock, NULL) != 0) {
            for (size_t initialized = 0u; initialized < index;
                 ++initialized) {
                (void)pthread_mutex_destroy(
                    &peers->peers[initialized].lock);
            }
            (void)signal(SIGPIPE, peers->previous_sigpipe);
            free(peers);
            return H2_PAL_ERR_IO;
        }
        peers->peers[index].listen_fd = -1;
        peers->peers[index].accepted_fd = -1;
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
        (void)pthread_mutex_lock(&peer->lock);
        int listener = peer->listen_fd;
        peer->listen_fd = -1;
        int accepted = peer->accepted_fd;
        (void)pthread_mutex_unlock(&peer->lock);
        if (listener >= 0) (void)close(listener);
        if (accepted >= 0) (void)shutdown(accepted, SHUT_RDWR);
    }
    for (size_t index = 0u; index < 5u; ++index) {
        local_peer_t *peer = &peers->peers[index];
        if (peer->started) {
            if (pthread_join(peer->thread, NULL) != 0 ||
                peer->status != H2_PAL_OK) result = H2_PAL_ERR_IO;
        }
    }
    for (size_t index = 0u; index < 5u; ++index) {
        (void)pthread_mutex_destroy(&peers->peers[index].lock);
    }
    if (peers->sigpipe_changed) {
        (void)signal(SIGPIPE, peers->previous_sigpipe);
    }
    free(peers->root_ca);
    free(peers->wrong_ca);
    free(peers);
    return result;
}
