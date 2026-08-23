#include "h2_webrtc_pion_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>

static void h2_webrtc_fixture_sleep_ms(long milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int h2_webrtc_fixture_connect(int port) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }
    struct timeval timeout = {.tv_sec = 10};
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_fd, (const struct sockaddr *)&address,
                sizeof(address)) != 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static int h2_webrtc_fixture_endpoint_port(const char *endpoint) {
    const char *separator = strrchr(endpoint, ':');
    if (separator == NULL || separator[1] == '\0') {
        return 0;
    }
    char *end = NULL;
    long port = strtol(separator + 1, &end, 10);
    return end != separator + 1 && *end == '\0' && port > 0 && port <= 65535
               ? (int)port
               : 0;
}

static int h2_webrtc_fixture_send_all(int socket_fd, const void *data,
                                      size_t len) {
    const uint8_t *cursor = (const uint8_t *)data;
    while (len != 0u) {
        ssize_t sent = send(socket_fd, cursor, len, 0);
        if (sent <= 0) {
            return -1;
        }
        cursor += (size_t)sent;
        len -= (size_t)sent;
    }
    return 0;
}

static int h2_webrtc_fixture_request(int port, const char *method,
                                     const char *path, const char *body,
                                     size_t body_len, const char *extra_header,
                                     char *response, size_t response_cap,
                                     size_t *response_len) {
    *response_len = 0u;
    int socket_fd = h2_webrtc_fixture_connect(port);
    if (socket_fd < 0) {
        return -1;
    }
    char header[512];
    int header_len = snprintf(
        header, sizeof(header),
        "%s %s HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n"
        "Content-Type: application/sdp\r\n"
        "%s"
        "Content-Length: %zu\r\n\r\n",
        method, path, extra_header == NULL ? "" : extra_header, body_len);
    if (header_len <= 0 || (size_t)header_len >= sizeof(header) ||
        h2_webrtc_fixture_send_all(socket_fd, header, (size_t)header_len) !=
            0 ||
        (body_len != 0u &&
         h2_webrtc_fixture_send_all(socket_fd, body, body_len) != 0)) {
        close(socket_fd);
        return -1;
    }
    size_t used = 0u;
    while (used + 1u < response_cap) {
        ssize_t received =
            recv(socket_fd, &response[used], response_cap - used - 1u, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            break;
        }
        used += (size_t)received;
    }
    close(socket_fd);
    response[used] = '\0';
    *response_len = used;
    return used == 0u ? -1 : 0;
}

int h2_webrtc_pion_fixture_start(h2_webrtc_pion_fixture_t *fixture,
                                 const char *server_path, const char *mode) {
    if (fixture == NULL || server_path == NULL || mode == NULL) {
        return -1;
    }
    memset(fixture, 0, sizeof(*fixture));
    (void)snprintf(fixture->turn_username, sizeof(fixture->turn_username), "%s",
                   "h2peer");
    (void)snprintf(fixture->turn_credential, sizeof(fixture->turn_credential),
                   "%s", "h2peer-secret");
    char mode_flag[64];
    int mode_flag_len =
        snprintf(mode_flag, sizeof(mode_flag), "--ice-mode=%s", mode);
    if (mode_flag_len <= 0 || (size_t)mode_flag_len >= sizeof(mode_flag)) {
        return -1;
    }
    int ready_pipe[2];
    if (pipe(ready_pipe) != 0) {
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return -1;
    }
    if (child == 0) {
        close(ready_pipe[0]);
        if (dup2(ready_pipe[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(ready_pipe[1]);
        execl(server_path, server_path, "--listen=127.0.0.1:0",
              "--stun-listen=127.0.0.1:0", "--turn-listen=127.0.0.1:0",
              "--candidate-ip=127.0.0.1", mode_flag, (char *)NULL);
        _exit(127);
    }
    close(ready_pipe[1]);
    fixture->pid = child;
    int flags = fcntl(ready_pipe[0], F_GETFL, 0);
    if (flags < 0 || fcntl(ready_pipe[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        close(ready_pipe[0]);
        h2_webrtc_pion_fixture_stop(fixture);
        return -1;
    }
    char ready[512] = {0};
    size_t ready_len = 0u;
    for (int attempt = 0; attempt < 400; ++attempt) {
        ssize_t received = read(ready_pipe[0], &ready[ready_len],
                                sizeof(ready) - ready_len - 1u);
        if (received > 0) {
            ready_len += (size_t)received;
            ready[ready_len] = '\0';
            if (strchr(ready, '\n') != NULL) {
                char ice_udp[64] = {0};
                char ice_tcp[64] = {0};
                if (sscanf(ready,
                           "H2_WEBRTC_TEST_SERVER_READY http=127.0.0.1:%d "
                           "stun=127.0.0.1:%d turn=127.0.0.1:%d ice_udp=%63s "
                           "ice_tcp=%63s mode=%23s",
                           &fixture->port, &fixture->stun_port,
                           &fixture->turn_port, ice_udp, ice_tcp,
                           fixture->mode) == 6) {
                    fixture->ice_udp_port =
                        h2_webrtc_fixture_endpoint_port(ice_udp);
                    fixture->ice_tcp_port =
                        h2_webrtc_fixture_endpoint_port(ice_tcp);
                    close(ready_pipe[0]);
                    return fixture->port > 0 && fixture->stun_port > 0 &&
                                   fixture->turn_port > 0 &&
                                   strcmp(fixture->mode, mode) == 0 &&
                                   (strcmp(mode, "tcp") == 0
                                        ? fixture->ice_udp_port == 0 &&
                                              fixture->ice_tcp_port > 0
                                        : fixture->ice_udp_port > 0) &&
                                   (strcmp(mode, "udp") == 0
                                        ? fixture->ice_tcp_port == 0
                                        : fixture->ice_tcp_port > 0)
                               ? 0
                               : -1;
                }
            }
        } else if (received == 0) {
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
        int status = 0;
        if (waitpid(child, &status, WNOHANG) == child) {
            fixture->pid = 0;
            close(ready_pipe[0]);
            return -1;
        }
        h2_webrtc_fixture_sleep_ms(50);
    }
    close(ready_pipe[0]);
    h2_webrtc_pion_fixture_stop(fixture);
    return -1;
}

static int h2_webrtc_pion_fixture_exchange_impl(
    h2_webrtc_pion_fixture_t *fixture, h2_pal_webrtc_str_t offer,
    char *answer, size_t answer_cap, size_t *answer_len, int relay_only,
    unsigned reverse_channels) {
    if (fixture == NULL || offer.data == NULL || answer == NULL ||
        answer_cap == 0u || answer_len == NULL) {
        return -1;
    }
    char filtered_offer[32768];
    const char *request_offer = offer.data;
    size_t request_offer_len = offer.len;
    if (relay_only) {
        size_t used = 0u;
        int relay_count = 0;
        const char *cursor = offer.data;
        const char *end = offer.data + offer.len;
        while (cursor < end) {
            const char *line_end = strstr(cursor, "\r\n");
            size_t line_len = line_end == NULL
                                  ? (size_t)(end - cursor)
                                  : (size_t)(line_end - cursor + 2);
            int candidate =
                line_len >= sizeof("a=candidate:") - 1u &&
                memcmp(cursor, "a=candidate:", sizeof("a=candidate:") - 1u) ==
                    0;
            static const char relay_text[] = " typ relay";
            int relay = 0;
            if (candidate && line_len >= sizeof(relay_text) - 1u) {
                for (size_t i = 0u; i + sizeof(relay_text) - 1u <= line_len;
                     ++i) {
                    if (memcmp(cursor + i, relay_text,
                               sizeof(relay_text) - 1u) == 0) {
                        relay = 1;
                        break;
                    }
                }
            }
            if (!candidate || relay) {
                if (used + line_len >= sizeof(filtered_offer)) {
                    return -1;
                }
                memcpy(filtered_offer + used, cursor, line_len);
                used += line_len;
                if (relay) {
                    relay_count++;
                }
            }
            cursor += line_len;
        }
        if (relay_count == 0) {
            return -1;
        }
        filtered_offer[used] = '\0';
        request_offer = filtered_offer;
        request_offer_len = used;
    }
    char response[32768];
    size_t response_len = 0u;
    char request_headers[80];
    int request_headers_len =
        snprintf(request_headers, sizeof(request_headers),
                 "X-H2-Reverse-Channels: %u\r\n%s", reverse_channels,
                 relay_only ? "X-H2-Relay-Only: 1\r\n" : "");
    if (request_headers_len <= 0 ||
        (size_t)request_headers_len >= sizeof(request_headers) ||
        h2_webrtc_fixture_request(
            fixture->port, "POST", "/offer", request_offer, request_offer_len,
            request_headers, response, sizeof(response), &response_len) != 0 ||
        strstr(response, " 200 ") == NULL) {
        return -1;
    }
    static const char session_header[] = "\r\nX-H2-Session-Id: ";
    char *session = strstr(response, session_header);
    if (session == NULL) {
        return -1;
    }
    session += sizeof(session_header) - 1u;
    char *session_end = strstr(session, "\r\n");
    if (session_end == NULL || session_end == session ||
        (size_t)(session_end - session) >= sizeof(fixture->session_id)) {
        return -1;
    }
    memcpy(fixture->session_id, session, (size_t)(session_end - session));
    fixture->session_id[session_end - session] = '\0';
    char *body = strstr(response, "\r\n\r\n");
    if (body == NULL) {
        return -1;
    }
    body += 4;
    size_t len = response_len - (size_t)(body - response);
    if (len >= answer_cap) {
        return -1;
    }
    memcpy(answer, body, len);
    answer[len] = '\0';
    *answer_len = len;
    return 0;
}

int h2_webrtc_pion_fixture_exchange(h2_webrtc_pion_fixture_t *fixture,
                                    h2_pal_webrtc_str_t offer, char *answer,
                                    size_t answer_cap, size_t *answer_len,
                                    int relay_only) {
    return h2_webrtc_pion_fixture_exchange_impl(
        fixture, offer, answer, answer_cap, answer_len, relay_only, 3u);
}

int h2_webrtc_pion_fixture_exchange_performance(
    h2_webrtc_pion_fixture_t *fixture, h2_pal_webrtc_str_t offer,
    char *answer, size_t answer_cap, size_t *answer_len) {
    return h2_webrtc_pion_fixture_exchange_impl(
        fixture, offer, answer, answer_cap, answer_len, 0, 0u);
}

static int h2_webrtc_fixture_json_uint64(const char *json, const char *name,
                                         unsigned long long *value) {
    char pattern[64];
    int pattern_len = snprintf(pattern, sizeof(pattern), "\"%s\":", name);
    if (pattern_len <= 0 || (size_t)pattern_len >= sizeof(pattern)) {
        return -1;
    }
    const char *found = strstr(json, pattern);
    return found != NULL && sscanf(found + pattern_len, "%llu", value) == 1
               ? 0
               : -1;
}

static int h2_webrtc_fixture_json_string(const char *json, const char *name,
                                         char *value, size_t value_cap) {
    char pattern[64];
    int pattern_len = snprintf(pattern, sizeof(pattern), "\"%s\":\"", name);
    if (pattern_len <= 0 || (size_t)pattern_len >= sizeof(pattern)) {
        return -1;
    }
    const char *start = strstr(json, pattern);
    if (start == NULL) {
        return -1;
    }
    start += pattern_len;
    const char *end = strchr(start, '"');
    if (end == NULL || (size_t)(end - start) >= value_cap) {
        return -1;
    }
    memcpy(value, start, (size_t)(end - start));
    value[end - start] = '\0';
    return 0;
}

int h2_webrtc_pion_fixture_turn_stats(const h2_webrtc_pion_fixture_t *fixture,
                                      h2_webrtc_turn_stats_t *stats) {
    if (fixture == NULL || stats == NULL) {
        return -1;
    }
    char response[2048];
    size_t response_len = 0u;
    if (h2_webrtc_fixture_request(fixture->port, "GET", "/turn-stats", NULL, 0u,
                                  NULL, response, sizeof(response),
                                  &response_len) != 0 ||
        strstr(response, " 200 ") == NULL) {
        return -1;
    }
    char *body = strstr(response, "\r\n\r\n");
    if (body == NULL) {
        return -1;
    }
    body += 4;
    return h2_webrtc_fixture_json_uint64(body, "allocations_created",
                                         &stats->allocations_created) == 0 &&
                   h2_webrtc_fixture_json_uint64(body, "allocations_deleted",
                                                 &stats->allocations_deleted) ==
                       0 &&
                   h2_webrtc_fixture_json_uint64(body, "permissions_created",
                                                 &stats->permissions_created) ==
                       0 &&
                   h2_webrtc_fixture_json_uint64(body, "channels_created",
                                                 &stats->channels_created) ==
                       0 &&
                   h2_webrtc_fixture_json_uint64(body, "relay_ingress",
                                                 &stats->relay_ingress) == 0 &&
                   h2_webrtc_fixture_json_uint64(body, "relay_egress",
                                                 &stats->relay_egress) == 0
               ? 0
               : -1;
}

int h2_webrtc_pion_fixture_channel_stats(
    const h2_webrtc_pion_fixture_t *fixture, h2_webrtc_channel_stats_t *stats) {
    if (fixture == NULL || stats == NULL || fixture->session_id[0] == '\0') {
        return -1;
    }
    char path[112];
    int path_len = snprintf(path, sizeof(path), "/session/%s/channel-stats",
                            fixture->session_id);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
        return -1;
    }
    char response[2048];
    size_t response_len = 0u;
    if (h2_webrtc_fixture_request(fixture->port, "GET", path, NULL, 0u, NULL,
                                  response, sizeof(response),
                                  &response_len) != 0 ||
        strstr(response, " 200 ") == NULL) {
        return -1;
    }
    char *body = strstr(response, "\r\n\r\n");
    if (body == NULL) {
        return -1;
    }
    body += 4;
    return h2_webrtc_fixture_json_uint64(body, "created", &stats->created) ==
                       0 &&
                   h2_webrtc_fixture_json_uint64(body, "opened",
                                                 &stats->opened) == 0 &&
                   h2_webrtc_fixture_json_uint64(body, "closed",
                                                 &stats->closed) == 0 &&
                   h2_webrtc_fixture_json_uint64(body, "current",
                                                 &stats->current) == 0 &&
                   h2_webrtc_fixture_json_uint64(body, "max_current",
                                                 &stats->max_current) == 0 &&
                   h2_webrtc_fixture_json_uint64(body, "reverse_replies",
                                                 &stats->reverse_replies) == 0
               ? 0
               : -1;
}

int h2_webrtc_pion_fixture_ice_pair(const h2_webrtc_pion_fixture_t *fixture,
                                    h2_webrtc_ice_pair_t *pair) {
    if (fixture == NULL || pair == NULL || fixture->session_id[0] == '\0') {
        return -1;
    }
    char path[112];
    int path_len = snprintf(path, sizeof(path), "/session/%s/ice-pair",
                            fixture->session_id);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
        return -1;
    }
    char response[2048];
    size_t response_len = 0u;
    if (h2_webrtc_fixture_request(fixture->port, "GET", path, NULL, 0u, NULL,
                                  response, sizeof(response),
                                  &response_len) != 0 ||
        strstr(response, " 200 ") == NULL) {
        return -1;
    }
    char *body = strstr(response, "\r\n\r\n");
    if (body == NULL) {
        return -1;
    }
    body += 4;
    memset(pair, 0, sizeof(*pair));
    return h2_webrtc_fixture_json_string(body, "mode", pair->mode,
                                         sizeof(pair->mode)) == 0 &&
                   h2_webrtc_fixture_json_string(
                       body, "local_protocol", pair->local_protocol,
                       sizeof(pair->local_protocol)) == 0 &&
                   h2_webrtc_fixture_json_string(
                       body, "remote_protocol", pair->remote_protocol,
                       sizeof(pair->remote_protocol)) == 0 &&
                   h2_webrtc_fixture_json_string(
                       body, "local_type", pair->local_type,
                       sizeof(pair->local_type)) == 0 &&
                   h2_webrtc_fixture_json_string(
                       body, "remote_type", pair->remote_type,
                       sizeof(pair->remote_type)) == 0 &&
                   h2_webrtc_fixture_json_string(
                       body, "local_tcp_type", pair->local_tcp_type,
                       sizeof(pair->local_tcp_type)) == 0 &&
                   h2_webrtc_fixture_json_string(
                       body, "remote_tcp_type", pair->remote_tcp_type,
                       sizeof(pair->remote_tcp_type)) == 0 &&
                   h2_webrtc_fixture_json_uint64(body, "udp_drops",
                                                 &pair->udp_drops) == 0
               ? 0
               : -1;
}

int h2_webrtc_pion_fixture_close_session(h2_webrtc_pion_fixture_t *fixture) {
    if (fixture == NULL || fixture->session_id[0] == '\0') {
        return -1;
    }
    char path[96];
    int path_len =
        snprintf(path, sizeof(path), "/session/%s/close", fixture->session_id);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
        return -1;
    }
    char response[1024];
    size_t response_len = 0u;
    int result =
        h2_webrtc_fixture_request(fixture->port, "POST", path, NULL, 0u, NULL,
                                  response, sizeof(response), &response_len);
    fixture->session_id[0] = '\0';
    return result == 0 && strstr(response, " 204 ") != NULL ? 0 : -1;
}

void h2_webrtc_pion_fixture_stop(h2_webrtc_pion_fixture_t *fixture) {
    if (fixture == NULL || fixture->pid <= 0) {
        return;
    }
    char response[1024];
    size_t response_len = 0u;
    (void)h2_webrtc_fixture_request(fixture->port, "POST", "/shutdown", NULL,
                                    0u, NULL, response, sizeof(response),
                                    &response_len);
    for (int attempt = 0; attempt < 50; ++attempt) {
        int status = 0;
        if (waitpid(fixture->pid, &status, WNOHANG) == fixture->pid) {
            fixture->pid = 0;
            return;
        }
        h2_webrtc_fixture_sleep_ms(100);
    }
    (void)kill(fixture->pid, SIGTERM);
    for (int attempt = 0; attempt < 10; ++attempt) {
        int status = 0;
        if (waitpid(fixture->pid, &status, WNOHANG) == fixture->pid) {
            fixture->pid = 0;
            return;
        }
        h2_webrtc_fixture_sleep_ms(100);
    }
    (void)kill(fixture->pid, SIGKILL);
    (void)waitpid(fixture->pid, NULL, 0);
    fixture->pid = 0;
}
