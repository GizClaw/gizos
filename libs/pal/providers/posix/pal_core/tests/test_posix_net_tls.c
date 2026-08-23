#include "h2_posix_pal_core.h"
#include "h2_wolfssl.h"

#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wolfssl/ssl.h>

typedef struct test_file {
    uint8_t *data;
    size_t len;
} test_file_t;

typedef struct tls_server {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint16_t port;
    int ready;
    int close_before_tls;
    const char *cert_path;
    const char *key_path;
} tls_server_t;

static const char *s_cert_path;
static const char *s_key_path;

static void *test_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static test_file_t read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long len = ftell(file);
    assert(len >= 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *data = (uint8_t *)malloc((size_t)len);
    assert(data != NULL);
    assert(fread(data, 1u, (size_t)len, file) == (size_t)len);
    fclose(file);
    return (test_file_t){ .data = data, .len = (size_t)len };
}

static void server_ready(tls_server_t *server, uint16_t port) {
    pthread_mutex_lock(&server->lock);
    server->port = port;
    server->ready = 1;
    pthread_cond_signal(&server->cond);
    pthread_mutex_unlock(&server->lock);
}

static void *tls_server_thread(void *arg) {
    tls_server_t *server = (tls_server_t *)arg;
    sigset_t sigpipe_mask;
    assert(sigemptyset(&sigpipe_mask) == 0);
    assert(sigaddset(&sigpipe_mask, SIGPIPE) == 0);
    assert(pthread_sigmask(SIG_BLOCK, &sigpipe_mask, NULL) == 0);
    WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    assert(ctx != NULL);
    assert(wolfSSL_CTX_use_certificate_file(
               ctx, server->cert_path, WOLFSSL_FILETYPE_PEM) ==
           WOLFSSL_SUCCESS);
    assert(wolfSSL_CTX_use_PrivateKey_file(
               ctx, server->key_path, WOLFSSL_FILETYPE_PEM) ==
           WOLFSSL_SUCCESS);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listen_fd >= 0);
    int reuse = 1;
    assert(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listen_fd, 1) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(listen_fd, (struct sockaddr *)&addr, &len) == 0);
    server_ready(server, ntohs(addr.sin_port));

    int fd = accept(listen_fd, NULL, NULL);
    assert(fd >= 0);
    if (!server->close_before_tls) {
        WOLFSSL *ssl = wolfSSL_new(ctx);
        assert(ssl != NULL);
        assert(wolfSSL_set_fd(ssl, fd) == WOLFSSL_SUCCESS);
        if (wolfSSL_accept(ssl) == WOLFSSL_SUCCESS) {
            uint8_t request[4];
            assert(wolfSSL_read(ssl, request, sizeof(request)) ==
                   (int)sizeof(request));
            assert(memcmp(request, "ping", sizeof(request)) == 0);
            static const uint8_t ok[] = { 'o', 'k' };
            assert(wolfSSL_write(ssl, ok, sizeof(ok)) == (int)sizeof(ok));
            (void)wolfSSL_shutdown(ssl);
        }
        wolfSSL_free(ssl);
    }
    close(fd);
    close(listen_fd);
    wolfSSL_CTX_free(ctx);
    return NULL;
}

static uint16_t start_server(tls_server_t *server, pthread_t *thread, int close_before_tls) {
    memset(server, 0, sizeof(*server));
    pthread_mutex_init(&server->lock, NULL);
    pthread_cond_init(&server->cond, NULL);
    server->cert_path = s_cert_path;
    server->key_path = s_key_path;
    server->close_before_tls = close_before_tls;
    assert(pthread_create(thread, NULL, tls_server_thread, server) == 0);
    pthread_mutex_lock(&server->lock);
    while (!server->ready) {
        pthread_cond_wait(&server->cond, &server->lock);
    }
    uint16_t port = server->port;
    pthread_mutex_unlock(&server->lock);
    return port;
}

static h2_pal_result_t connect_tls(uint16_t port, const test_file_t *ca, const char *server_name) {
    const h2_pal_net_api_t *net = h2_posix_net_api();
    h2_pal_net_socket_t socket_fd = -1;
    h2_pal_result_t rc = h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL, &socket_fd);
    assert(rc == H2_PAL_OK);
    h2_pal_net_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = H2_PAL_NET_FAMILY_IPV4;
    addr.port = port;
    addr.ip[0] = 127u;
    addr.ip[3] = 1u;
    rc = h2_pal_net_tcp_connect(net, socket_fd, &addr, 1000u);
    assert(rc == H2_PAL_OK);
    h2_pal_net_tls_config_t tls;
    memset(&tls, 0, sizeof(tls));
    tls.server_name = server_name;
    tls.root_ca_pem = ca->data;
    tls.root_ca_pem_len = ca->len;
    tls.verify = H2_PAL_NET_TLS_VERIFY_REQUIRED;
    h2_pal_net_socket_t tls_socket = -1;
    rc = h2_pal_net_tls_wrap(net, socket_fd, &tls, 1000u, &tls_socket);
    if (rc == H2_PAL_OK) {
        assert(h2_pal_net_tcp_send_timeout(
                   net, tls_socket, (const uint8_t *)"ping", 4u, 1000u) == 4);
        uint8_t data[2];
        assert(h2_pal_net_tcp_recv(net, tls_socket, data, sizeof(data), 1000u) == 2);
        assert(memcmp(data, "ok", 2u) == 0);
    }
    h2_pal_net_close(net, socket_fd);
    return rc;
}

static void run_case(const test_file_t *ca, const char *server_name, h2_pal_result_t expected, int close_before_tls) {
    tls_server_t server;
    pthread_t thread;
    uint16_t port = start_server(&server, &thread, close_before_tls);
    h2_pal_result_t rc = connect_tls(port, ca, server_name);
    if (rc != expected) {
        fprintf(
            stderr,
            "TLS case %s returned %d, expected %d\n",
            server_name,
            rc,
            expected);
    }
    assert(rc == expected);
    assert(pthread_join(thread, NULL) == 0);
    pthread_mutex_destroy(&server.lock);
    pthread_cond_destroy(&server.cond);
}

int main(int argc, char **argv) {
    assert(argc == 1 || argc == 5);
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    const char *ca_path = argc == 5 ? argv[1] : "tests/fixtures/localhost_ca.pem";
    const char *wrong_ca_path =
        argc == 5 ? argv[2] : "tests/fixtures/wrong_ca.pem";
    s_cert_path = argc == 5 ? argv[3] : "tests/fixtures/localhost_cert.pem";
    s_key_path = argc == 5 ? argv[4] : "tests/fixtures/localhost_key.pem";
    test_file_t ca = read_file(ca_path);
    test_file_t wrong_ca = read_file(wrong_ca_path);
    const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_alloc,
        .realloc = test_realloc,
        .free = test_free,
    };
    const h2_wolfssl_config_t wolfssl = {
        .mem = {.user = NULL, .vtable = &mem_vtable},
        .entropy_user = NULL,
        .entropy = h2_posix_entropy,
    };
    assert(h2_wolfssl_init(&wolfssl) == H2_PAL_OK);

    h2_pal_net_tls_config_t tls;
    memset(&tls, 0, sizeof(tls));
    h2_pal_net_socket_t out_socket = -1;
    assert(h2_pal_net_tls_wrap(NULL, 0, &tls, 1u, &out_socket) == H2_PAL_ERR_INVALID_ARG);
    h2_pal_net_api_t no_tls = *h2_posix_net_api();
    h2_pal_net_vtable_t no_tls_vtable = *no_tls.vtable;
    no_tls_vtable.tls_wrap = NULL;
    no_tls.vtable = &no_tls_vtable;
    assert(h2_pal_net_tls_wrap(&no_tls, 0, &tls, 1u, &out_socket) == H2_PAL_ERR_UNSUPPORTED);

    run_case(&ca, "localhost", H2_PAL_OK, 0);
    run_case(&wrong_ca, "localhost", H2_PAL_ERR_TLS_VERIFY, 0);
    run_case(&ca, "not-localhost", H2_PAL_ERR_TLS_VERIFY, 0);
    run_case(&ca, "localhost", H2_PAL_ERR_IO, 1);

    free(ca.data);
    free(wrong_ca.data);
    assert(h2_wolfssl_deinit() == H2_PAL_OK);
    return 0;
}
