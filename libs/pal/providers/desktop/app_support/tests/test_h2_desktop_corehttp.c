#include "h2_corehttp.h"
#include "h2_desktop_app_support_c.h"
#include "h2_desktop_platform.h"

#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <wolfssl/ssl.h>

typedef struct test_file {
    uint8_t *data;
    size_t len;
} test_file_t;

typedef enum http_server_scenario {
    HTTP_SERVER_METHODS,
    HTTP_SERVER_REDIRECT,
    HTTP_SERVER_STALL,
    HTTP_SERVER_TLS_SUCCESS,
    HTTP_SERVER_TLS_REJECT,
    HTTP_SERVER_TLS_INTERRUPT,
} http_server_scenario_t;

typedef struct http_server {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint16_t port;
    int ready;
    http_server_scenario_t scenario;
    size_t connection_count;
    const char *cert_path;
    const char *key_path;
} http_server_t;

static test_file_t read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long len = ftell(file);
    assert(len > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *data = malloc((size_t)len);
    assert(data != NULL);
    assert(fread(data, 1u, (size_t)len, file) == (size_t)len);
    assert(fclose(file) == 0);
    return (test_file_t){.data = data, .len = (size_t)len};
}

static void publish_port(http_server_t *server, uint16_t port) {
    assert(pthread_mutex_lock(&server->lock) == 0);
    server->port = port;
    server->ready = 1;
    assert(pthread_cond_signal(&server->cond) == 0);
    assert(pthread_mutex_unlock(&server->lock) == 0);
}

static void read_request(int fd, char *request, size_t capacity) {
    assert(request != NULL && capacity > 1u);
    size_t used = 0u;
    while (used + 1u < capacity && strstr(request, "\r\n\r\n") == NULL) {
        ssize_t got = recv(fd, request + used, capacity - used - 1u, 0);
        assert(got > 0);
        used += (size_t)got;
        request[used] = '\0';
    }
}

static void send_response(int fd, const char *response) {
    size_t len = strlen(response);
    assert(send(fd, response, len, 0) == (ssize_t)len);
}

static void serve_raw_http(
    const http_server_t *server, int fd, size_t connection_index) {
    char request[1024] = {0};
    read_request(fd, request, sizeof(request));
    if (server->scenario == HTTP_SERVER_METHODS) {
        static const char *const methods[] = {
            "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS",
        };
        assert(connection_index < sizeof(methods) / sizeof(methods[0]));
        char request_line[64];
        int len = snprintf(request_line, sizeof(request_line),
                           "%s /method HTTP/1.1\r\n",
                           methods[connection_index]);
        assert(len > 0 && (size_t)len < sizeof(request_line));
        assert(strstr(request, request_line) == request);
        assert(strstr(request, "X-Desktop-Test: yes\r\n") != NULL);
        send_response(fd, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
        return;
    }
    if (server->scenario == HTTP_SERVER_REDIRECT) {
        if (connection_index == 0u) {
            assert(strstr(request, "GET /redirect HTTP/1.1\r\n") == request);
            send_response(
                fd, "HTTP/1.1 302 Found\r\nLocation: /final\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n");
        } else {
            assert(connection_index == 1u);
            assert(strstr(request, "GET /final HTTP/1.1\r\n") == request);
            send_response(
                fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                    "Connection: close\r\n\r\n3\r\nred\r\n5\r\nirect\r\n0\r\n\r\n");
        }
        return;
    }
    assert(server->scenario == HTTP_SERVER_STALL);
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 250000000};
    assert(nanosleep(&delay, NULL) == 0);
}

static void serve_tls_http(WOLFSSL_CTX *ctx, int fd,
                           http_server_scenario_t scenario) {
    WOLFSSL *ssl = wolfSSL_new(ctx);
    assert(ssl != NULL);
    assert(wolfSSL_set_fd(ssl, fd) == WOLFSSL_SUCCESS);
    int accept_result = wolfSSL_accept(ssl);
    if (scenario == HTTP_SERVER_TLS_REJECT) {
        if (accept_result == WOLFSSL_SUCCESS) {
            (void)wolfSSL_shutdown(ssl);
        }
        wolfSSL_free(ssl);
        return;
    }
    assert(accept_result == WOLFSSL_SUCCESS);
    char request[1024] = {0};
    size_t used = 0u;
    while (used + 1u < sizeof(request) && strstr(request, "\r\n\r\n") == NULL) {
        int got = wolfSSL_read(ssl, request + used,
                               (int)(sizeof(request) - used - 1u));
        assert(got > 0);
        used += (size_t)got;
        request[used] = '\0';
    }
    assert(strstr(request, "GET /desktop HTTP/1.1\r\n") == request);
    static const char response[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n7\r\ndesktop\r\n0\r\n\r\n";
    assert(wolfSSL_write(ssl, response, sizeof(response) - 1u) ==
           (int)(sizeof(response) - 1u));
    (void)wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
}

static void *server_thread(void *arg) {
    http_server_t *server = arg;
    WOLFSSL_CTX *ctx = NULL;
    if (server->scenario == HTTP_SERVER_TLS_SUCCESS ||
        server->scenario == HTTP_SERVER_TLS_REJECT) {
        ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
        assert(ctx != NULL);
        assert(wolfSSL_CTX_use_certificate_file(
                   ctx, server->cert_path, WOLFSSL_FILETYPE_PEM) ==
               WOLFSSL_SUCCESS);
        assert(wolfSSL_CTX_use_PrivateKey_file(
                   ctx, server->key_path, WOLFSSL_FILETYPE_PEM) ==
               WOLFSSL_SUCCESS);
    }
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listen_fd >= 0);
    int reuse = 1;
    assert(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                      sizeof(reuse)) == 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listen_fd, (int)server->connection_count) == 0);
    socklen_t address_len = sizeof(address);
    assert(getsockname(listen_fd, (struct sockaddr *)&address, &address_len) == 0);
    publish_port(server, ntohs(address.sin_port));
    for (size_t index = 0u; index < server->connection_count; ++index) {
        int fd = accept(listen_fd, NULL, NULL);
        assert(fd >= 0);
        if (server->scenario == HTTP_SERVER_TLS_INTERRUPT) {
            assert(close(fd) == 0);
            continue;
        }
        if (ctx == NULL) {
            serve_raw_http(server, fd, index);
        } else {
            serve_tls_http(ctx, fd, server->scenario);
        }
        assert(close(fd) == 0);
    }
    assert(close(listen_fd) == 0);
    wolfSSL_CTX_free(ctx);
    return NULL;
}

static uint16_t start_server(http_server_t *server, pthread_t *thread,
                             http_server_scenario_t scenario,
                             size_t connection_count, const char *cert_path,
                             const char *key_path) {
    memset(server, 0, sizeof(*server));
    assert(pthread_mutex_init(&server->lock, NULL) == 0);
    assert(pthread_cond_init(&server->cond, NULL) == 0);
    server->scenario = scenario;
    server->connection_count = connection_count;
    server->cert_path = cert_path;
    server->key_path = key_path;
    assert(pthread_create(thread, NULL, server_thread, server) == 0);
    assert(pthread_mutex_lock(&server->lock) == 0);
    while (!server->ready)
        assert(pthread_cond_wait(&server->cond, &server->lock) == 0);
    uint16_t port = server->port;
    assert(pthread_mutex_unlock(&server->lock) == 0);
    return port;
}

static void finish_server(http_server_t *server, pthread_t thread) {
    assert(pthread_join(thread, NULL) == 0);
    assert(pthread_mutex_destroy(&server->lock) == 0);
    assert(pthread_cond_destroy(&server->cond) == 0);
}

static void run_methods(const h2_pal_http_api_t *http) {
    static const h2_pal_http_method_t methods[] = {
        H2_PAL_HTTP_GET, H2_PAL_HTTP_POST, H2_PAL_HTTP_PUT,
        H2_PAL_HTTP_PATCH, H2_PAL_HTTP_DELETE, H2_PAL_HTTP_HEAD,
        H2_PAL_HTTP_OPTIONS,
    };
    http_server_t server;
    pthread_t thread;
    uint16_t port = start_server(
        &server, &thread, HTTP_SERVER_METHODS,
        sizeof(methods) / sizeof(methods[0]), NULL, NULL);
    char url[128];
    int url_len = snprintf(
        url, sizeof(url), "http://localhost:%u/method", port);
    assert(url_len > 0 && (size_t)url_len < sizeof(url));
    const h2_pal_http_header_t header = {
        .name = {.data = "X-Desktop-Test", .len = 14u},
        .value = {.data = "yes", .len = 3u},
    };
    for (size_t index = 0u; index < sizeof(methods) / sizeof(methods[0]);
         ++index) {
        const h2_pal_http_request_t request = {
            .method = methods[index],
            .url = {.data = url, .len = (size_t)url_len},
            .headers = &header,
            .header_count = 1u,
            .timeout_ms = 3000,
        };
        h2_pal_http_response_t response;
        assert(h2_pal_http_request(http, &request, &response) == H2_PAL_OK);
        assert(response.status_code == 204);
    }
    finish_server(&server, thread);
}

typedef struct stream_result {
    uint8_t body[16];
    size_t len;
} stream_result_t;

static int collect_stream(
    void *user,
    const h2_pal_http_request_t *request,
    const uint8_t *chunk,
    size_t chunk_len,
    size_t total_read,
    size_t remaining) {
    (void)request;
    stream_result_t *result = user;
    assert(total_read == result->len + chunk_len);
    assert(remaining == 0u);
    assert(chunk_len <= sizeof(result->body) - result->len);
    memcpy(result->body + result->len, chunk, chunk_len);
    result->len += chunk_len;
    return H2_PAL_OK;
}

static void run_redirect_stream(const h2_pal_http_api_t *http) {
    http_server_t server;
    pthread_t thread;
    uint16_t port = start_server(
        &server, &thread, HTTP_SERVER_REDIRECT, 2u, NULL, NULL);
    char url[128];
    int url_len = snprintf(
        url, sizeof(url), "http://localhost:%u/redirect", port);
    assert(url_len > 0 && (size_t)url_len < sizeof(url));
    uint8_t chunk_buf[2];
    stream_result_t stream = {0};
    const h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = url, .len = (size_t)url_len},
        .timeout_ms = 3000,
        .chunk_buf = chunk_buf,
        .chunk_buf_cap = sizeof(chunk_buf),
        .read_cb = collect_stream,
        .user = &stream,
    };
    h2_pal_http_response_t response;
    assert(h2_pal_http_request(http, &request, &response) == H2_PAL_OK);
    assert(response.status_code == 200);
    assert(stream.len == 8u);
    assert(memcmp(stream.body, "redirect", 8u) == 0);
    finish_server(&server, thread);
}

static int cancel_after_checks(void *user) {
    size_t *checks = user;
    *checks += 1u;
    return *checks >= 5u;
}

static void run_stall(const h2_pal_http_api_t *http, int cancel) {
    http_server_t server;
    pthread_t thread;
    uint16_t port = start_server(
        &server, &thread, HTTP_SERVER_STALL, 1u, NULL, NULL);
    char url[128];
    int url_len = snprintf(
        url, sizeof(url), "http://localhost:%u/stall", port);
    assert(url_len > 0 && (size_t)url_len < sizeof(url));
    size_t checks = 0u;
    const h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = url, .len = (size_t)url_len},
        .timeout_ms = cancel ? 3000 : 50,
        .cancel_cb = cancel ? cancel_after_checks : NULL,
        .cancel_user = cancel ? &checks : NULL,
    };
    h2_pal_http_response_t response;
    assert(h2_pal_http_request(http, &request, &response) ==
           (cancel ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_TIMEOUT));
    finish_server(&server, thread);
}

static void run_request(const h2_pal_http_api_t *http, uint16_t port,
                        const char *host, int use_tls) {
    char url[128];
    int len = snprintf(url, sizeof(url), "%s://%s:%u/desktop",
                       use_tls ? "https" : "http", host, port);
    assert(len > 0 && (size_t)len < sizeof(url));
    uint8_t response_body[16] = {0};
    const h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = url, .len = (size_t)len},
        .timeout_ms = 3000,
        .response_buf = response_body,
        .response_buf_cap = sizeof(response_body),
    };
    h2_pal_http_response_t response;
    assert(h2_pal_http_request(http, &request, &response) == H2_PAL_OK);
    assert(response.status_code == 200);
    assert(response.body == response_body);
    assert(response.body_len == 7u);
    assert(memcmp(response.body, "desktop", 7u) == 0);
    h2_pal_http_response_free(http, &response);
}

static void run_rejected_tls_request(
    const h2_pal_http_api_t *http,
    http_server_scenario_t scenario,
    const char *host,
    const char *cert_path,
    const char *key_path,
    h2_pal_result_t expected) {
    http_server_t server;
    pthread_t thread;
    uint16_t port = start_server(
        &server, &thread, scenario, 1u, cert_path, key_path);
    char url[128];
    int len = snprintf(
        url, sizeof(url), "https://%s:%u/desktop", host, port);
    assert(len > 0 && (size_t)len < sizeof(url));
    uint8_t response_body[16] = {0};
    const h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = url, .len = (size_t)len},
        .timeout_ms = 3000,
        .response_buf = response_body,
        .response_buf_cap = sizeof(response_body),
    };
    h2_pal_http_response_t response;
    memset(&response, 0xa5, sizeof(response));
    assert(h2_pal_http_request(http, &request, &response) == expected);
    assert(response.status_code == 0);
    assert(response.body == NULL);
    assert(response.body_len == 0u);
    finish_server(&server, thread);
}

int main(int argc, char **argv) {
    assert(argc == 5);
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    h2_desktop_network_services_t *network = NULL;
    assert(h2_desktop_network_services_create(0, 0, &network) == H2_PAL_OK);
    test_file_t ca = read_file(argv[1]);
    test_file_t wrong_ca = read_file(argv[2]);
    h2_corehttp_config_t config = {
        .allocator = h2_desktop_platform_default_allocator(),
        .net = h2_desktop_host_net_api(),
        .time = h2_desktop_platform_time_api(),
        .root_ca_pem = ca.data,
        .root_ca_pem_len = ca.len,
        .io_slice_ms = 20u,
    };
    h2_corehttp_t *provider = NULL;
    h2_pal_http_api_t http = {0};
    assert(h2_corehttp_create(&config, &provider, &http) == H2_PAL_OK);
    run_methods(&http);
    run_redirect_stream(&http);
    run_stall(&http, 1);
    run_stall(&http, 0);
    http_server_t server;
    pthread_t thread;
    uint16_t port = start_server(
        &server, &thread, HTTP_SERVER_TLS_SUCCESS, 1u, argv[3], argv[4]);
    run_request(&http, port, "localhost", 1);
    finish_server(&server, thread);
    h2_corehttp_destroy(provider);

    config.root_ca_pem = wrong_ca.data;
    config.root_ca_pem_len = wrong_ca.len;
    assert(h2_corehttp_create(&config, &provider, &http) == H2_PAL_OK);
    run_rejected_tls_request(
        &http, HTTP_SERVER_TLS_REJECT, "localhost", argv[3], argv[4],
        H2_PAL_ERR_TLS_VERIFY);
    h2_corehttp_destroy(provider);

    config.root_ca_pem = ca.data;
    config.root_ca_pem_len = ca.len;
    assert(h2_corehttp_create(&config, &provider, &http) == H2_PAL_OK);
    run_rejected_tls_request(
        &http, HTTP_SERVER_TLS_REJECT, "127.0.0.1", argv[3], argv[4],
        H2_PAL_ERR_TLS_VERIFY);
    run_rejected_tls_request(
        &http, HTTP_SERVER_TLS_INTERRUPT, "localhost", argv[3], argv[4],
        H2_PAL_ERR_IO);
    h2_corehttp_destroy(provider);
    free(ca.data);
    free(wrong_ca.data);
    h2_desktop_network_services_destroy(network);
    return 0;
}
