#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include "h2_desktop_platform.h"
#include "h2_desktop_app_support_c.h"
#include "h2_pal_e2e.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(H2_MQTT_SMOKE_LOOPBACK)
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#endif

#define DEFAULT_HOST "broker.emqx.io"
#define DEFAULT_TCP_PORT 1883u
#define DEFAULT_TLS_PORT 8883u
#define DEFAULT_TOPIC_PREFIX "h2/public-smoke"
#define DEFAULT_TIMEOUT_MS 5000u
#define RANDOM_BYTES 16u
#define RANDOM_HEX_LEN (RANDOM_BYTES * 2u)

typedef struct smoke_config {
    const char *host;
    uint16_t port;
    int tls;
    const char *topic_prefix;
    uint32_t timeout_ms;
} smoke_config_t;

#if defined(H2_MQTT_SMOKE_LOOPBACK)
#define TEST_CHECK(expression) \
    do { \
        if (!(expression)) { \
            fprintf(stderr, "test check failed: %s (%s:%d)\n", #expression, __FILE__, __LINE__); \
            abort(); \
        } \
    } while (0)

typedef struct loopback_broker {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint16_t port;
    int ready;
    int status;
} loopback_broker_t;

static int socket_read_exact(int fd, uint8_t *data, size_t len) {
    size_t offset = 0u;
    while (offset < len) {
        ssize_t received = recv(fd, data + offset, len - offset, 0);
        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int socket_write_exact(int fd, const uint8_t *data, size_t len) {
    size_t offset = 0u;
    while (offset < len) {
        ssize_t sent = send(fd, data + offset, len - offset, 0);
        if (sent > 0) {
            offset += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int read_mqtt_packet(int fd, uint8_t *packet, size_t capacity, size_t *out_len) {
    if (capacity < 2u || socket_read_exact(fd, packet, 1u) != 0) {
        return -1;
    }
    size_t header_len = 1u;
    size_t remaining_len = 0u;
    size_t multiplier = 1u;
    uint8_t encoded = 0u;
    do {
        if (header_len >= 5u || socket_read_exact(fd, &encoded, 1u) != 0) {
            return -1;
        }
        packet[header_len++] = encoded;
        remaining_len += (size_t)(encoded & 0x7fu) * multiplier;
        multiplier *= 128u;
    } while ((encoded & 0x80u) != 0u);
    if (remaining_len > capacity - header_len ||
        socket_read_exact(fd, packet + header_len, remaining_len) != 0) {
        return -1;
    }
    *out_len = header_len + remaining_len;
    return (int)(packet[0] >> 4u);
}

static size_t mqtt_header_len(const uint8_t *packet, size_t packet_len) {
    size_t header_len = 1u;
    while (header_len < packet_len && header_len < 5u) {
        uint8_t encoded = packet[header_len++];
        if ((encoded & 0x80u) == 0u) {
            return header_len;
        }
    }
    return 0u;
}

static void loopback_broker_ready(loopback_broker_t *broker, uint16_t port) {
    TEST_CHECK(pthread_mutex_lock(&broker->lock) == 0);
    broker->port = port;
    broker->ready = 1;
    TEST_CHECK(pthread_cond_signal(&broker->cond) == 0);
    TEST_CHECK(pthread_mutex_unlock(&broker->lock) == 0);
}

static uint64_t monotonic_ms(void);

static int wait_for_connection(int listen_fd, uint32_t timeout_ms) {
    uint64_t deadline_ms = monotonic_ms() + timeout_ms;
    for (;;) {
        uint64_t now_ms = monotonic_ms();
        if (now_ms >= deadline_ms) {
            return 0;
        }
        uint64_t remaining_ms = deadline_ms - now_ms;
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        struct timeval timeout = {
            .tv_sec = (time_t)(remaining_ms / 1000u),
            .tv_usec = (suseconds_t)((remaining_ms % 1000u) * 1000u),
        };
        int result = select(listen_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (result >= 0 || errno != EINTR) {
            return result;
        }
    }
}

static void *loopback_broker_thread(void *user) {
    loopback_broker_t *broker = (loopback_broker_t *)user;
    broker->status = 1;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_CHECK(listen_fd >= 0);
    int reuse = 1;
    TEST_CHECK(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    TEST_CHECK(bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    TEST_CHECK(listen(listen_fd, 1) == 0);
    socklen_t address_len = sizeof(address);
    TEST_CHECK(getsockname(listen_fd, (struct sockaddr *)&address, &address_len) == 0);
    loopback_broker_ready(broker, ntohs(address.sin_port));

    if (wait_for_connection(listen_fd, 3000u) <= 0) {
        close(listen_fd);
        return NULL;
    }
    int fd = accept(listen_fd, NULL, NULL);
    TEST_CHECK(fd >= 0);
    uint8_t packet[4096];
    for (;;) {
        size_t packet_len = 0u;
        int packet_type = read_mqtt_packet(fd, packet, sizeof(packet), &packet_len);
        if (packet_type == 1) {
            static const uint8_t connack[] = { 0x20u, 0x02u, 0x00u, 0x00u };
            if (socket_write_exact(fd, connack, sizeof(connack)) != 0) {
                break;
            }
        } else if (packet_type == 8) {
            size_t header_len = mqtt_header_len(packet, packet_len);
            if (header_len == 0u || packet_len - header_len < 2u) {
                break;
            }
            uint8_t suback[] = {
                0x90u,
                0x03u,
                packet[header_len],
                packet[header_len + 1u],
                0x00u,
            };
            if (socket_write_exact(fd, suback, sizeof(suback)) != 0) {
                break;
            }
        } else if (packet_type == 3) {
            if (socket_write_exact(fd, packet, packet_len) != 0) {
                break;
            }
        } else if (packet_type == 14) {
            broker->status = 0;
            break;
        } else {
            break;
        }
    }
    close(fd);
    close(listen_fd);
    return NULL;
}

static uint16_t start_loopback_broker(loopback_broker_t *broker, pthread_t *thread) {
    memset(broker, 0, sizeof(*broker));
    TEST_CHECK(pthread_mutex_init(&broker->lock, NULL) == 0);
    TEST_CHECK(pthread_cond_init(&broker->cond, NULL) == 0);
    TEST_CHECK(pthread_create(thread, NULL, loopback_broker_thread, broker) == 0);
    TEST_CHECK(pthread_mutex_lock(&broker->lock) == 0);
    while (!broker->ready) {
        TEST_CHECK(pthread_cond_wait(&broker->cond, &broker->lock) == 0);
    }
    uint16_t port = broker->port;
    TEST_CHECK(pthread_mutex_unlock(&broker->lock) == 0);
    return port;
}
#endif

#if defined(H2_MQTT_SMOKE_LOOPBACK)
static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return ((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u);
}
#endif

#if !defined(H2_MQTT_SMOKE_LOOPBACK)
static int parse_bool(const char *value, int *out_bool) {
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0 ||
        strcmp(value, "yes") == 0 || strcmp(value, "YES") == 0 || strcmp(value, "on") == 0 ||
        strcmp(value, "ON") == 0) {
        *out_bool = 1;
        return 0;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "FALSE") == 0 ||
        strcmp(value, "no") == 0 || strcmp(value, "NO") == 0 || strcmp(value, "off") == 0 ||
        strcmp(value, "OFF") == 0) {
        *out_bool = 0;
        return 0;
    }
    return -1;
}

static int parse_u32_env(const char *name, uint32_t min, uint32_t max, uint32_t *out_value) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < min || parsed > max) {
        fprintf(stderr, "%s must be an integer in [%u, %u], got '%s'\n", name, min, max, value);
        return -1;
    }
    *out_value = (uint32_t)parsed;
    return 0;
}

static int load_smoke_config(smoke_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->host = getenv("H2_MQTT_SMOKE_HOST");
    if (config->host == NULL || config->host[0] == '\0') {
        config->host = DEFAULT_HOST;
    }
    config->topic_prefix = getenv("H2_MQTT_SMOKE_TOPIC_PREFIX");
    if (config->topic_prefix == NULL || config->topic_prefix[0] == '\0') {
        config->topic_prefix = DEFAULT_TOPIC_PREFIX;
    }
    config->timeout_ms = DEFAULT_TIMEOUT_MS;

    const char *tls_env = getenv("H2_MQTT_SMOKE_TLS");
    if (tls_env != NULL && tls_env[0] != '\0' && parse_bool(tls_env, &config->tls) != 0) {
        fprintf(stderr, "H2_MQTT_SMOKE_TLS must be 0/1, true/false, yes/no, or on/off, got '%s'\n", tls_env);
        return -1;
    }

    uint32_t port = config->tls ? DEFAULT_TLS_PORT : DEFAULT_TCP_PORT;
    if (parse_u32_env("H2_MQTT_SMOKE_PORT", 1u, UINT16_MAX, &port) != 0) {
        return -1;
    }
    config->port = (uint16_t)port;

    if (parse_u32_env("H2_MQTT_SMOKE_TIMEOUT_MS", 1u, 60000u, &config->timeout_ms) != 0) {
        return -1;
    }
    return 0;
}
#endif

static int fill_random(uint8_t *data, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "failed to open /dev/urandom: %s\n", strerror(errno));
        return -1;
    }

    size_t offset = 0u;
    while (offset < len) {
        ssize_t n = read(fd, data + offset, len - offset);
        if (n > 0) {
            offset += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        fprintf(stderr, "failed to read /dev/urandom: %s\n", n < 0 ? strerror(errno) : "short read");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static void hex_encode(const uint8_t *data, size_t len, char *out_hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0u; i < len; ++i) {
        out_hex[i * 2u] = digits[data[i] >> 4u];
        out_hex[(i * 2u) + 1u] = digits[data[i] & 0x0fu];
    }
    out_hex[len * 2u] = '\0';
}

static int make_smoke_strings(
    const smoke_config_t *config,
    char *client_id,
    size_t client_id_len,
    char *topic,
    size_t topic_len,
    char *payload,
    size_t payload_len) {
    uint8_t random[RANDOM_BYTES];
    char token[RANDOM_HEX_LEN + 1u];
    if (fill_random(random, sizeof(random)) != 0) {
        return -1;
    }
    hex_encode(random, sizeof(random), token);

    int written = snprintf(client_id, client_id_len, "h2-smoke-%s", token);
    if (written < 0 || (size_t)written >= client_id_len) {
        fprintf(stderr, "client id buffer is too small\n");
        return -1;
    }
    written = snprintf(topic, topic_len, "%s/%s", config->topic_prefix, token);
    if (written < 0 || (size_t)written >= topic_len) {
        fprintf(stderr, "topic buffer is too small for prefix '%s'\n", config->topic_prefix);
        return -1;
    }
    written = snprintf(payload, payload_len, "h2-smoke-payload-%s", token);
    if (written < 0 || (size_t)written >= payload_len) {
        fprintf(stderr, "payload buffer is too small\n");
        return -1;
    }
    return 0;
}

static h2_runtime_config_t make_runtime_config(
    h2_desktop_network_services_t *network) {
    h2_runtime_config_t config;
    memset(&config, 0, sizeof(config));
    config.board = "desktop";
    config.target = "host";
    config.chip = "host";
    config.firmware_info = h2_pal_unsupported_firmware_info_api();
    config.mem = h2_desktop_platform_default_allocator();
    config.log = h2_desktop_platform_log_api();
    config.time = h2_desktop_platform_time_api();
    config.timer = h2_pal_unsupported_timer_api();
    config.task = h2_desktop_platform_task_api();
    config.queue = h2_desktop_platform_queue_api();
    config.sync = h2_desktop_platform_sync_api();
    config.fs = h2_pal_unsupported_fs_api();
    config.disk = h2_pal_unsupported_disk_api();
    config.pref = h2_pal_unsupported_pref_api();
    config.crypto = h2_pal_unsupported_crypto_api();
    config.http = h2_pal_unsupported_http_api();
    config.net = h2_desktop_host_net_api();
    config.netif = h2_pal_unsupported_netif_api();
    config.mqtt = h2_desktop_network_services_mqtt(network);
    config.webrtc = h2_pal_unsupported_webrtc_api();
    config.wifi_sta = h2_pal_unsupported_wifi_sta_api();
    config.wifi_ap = h2_pal_unsupported_wifi_ap_api();
    config.wifi_csi = h2_pal_unsupported_wifi_csi_api();
    config.wifi_settings = h2_pal_unsupported_wifi_settings_api();
    config.ble_host = h2_pal_unsupported_ble_host_api();
    config.modem = h2_pal_unsupported_modem_api();
    config.power = h2_pal_unsupported_power_api();
    config.display = h2_pal_unsupported_display_api();
    config.audio = h2_pal_unsupported_audio_api();
    config.audio_decoder = h2_pal_unsupported_audio_decoder_api();
    config.periph = h2_pal_unsupported_periph_api();
    config.button = h2_pal_unsupported_button_api();
    config.touch = h2_pal_unsupported_touch_api();
    config.buzzer = h2_pal_unsupported_buzzer_api();
    config.nfc = h2_pal_unsupported_nfc_api();
    config.nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api();
    config.imu = h2_pal_unsupported_imu_api();
    config.gpio_irq = h2_pal_unsupported_gpio_irq_api();
    config.led = h2_pal_unsupported_led_api();
    config.switch_api = h2_pal_unsupported_switch_api();
    config.pwm_switch = h2_pal_unsupported_pwm_switch_api();
    config.input = h2_pal_unsupported_input_api();
    config.system_event = h2_pal_unsupported_system_event_api();
    config.video_decoder = h2_pal_unsupported_video_decoder_api();
    config.event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
    return config;
}

static int run_smoke_with_config(const smoke_config_t *smoke) {
    char client_id[64];
    char topic[160];
    char payload[96];
    if (make_smoke_strings(smoke, client_id, sizeof(client_id), topic, sizeof(topic), payload, sizeof(payload)) != 0) {
        return 1;
    }

    fprintf(
        stderr,
        "MQTT smoke: host=%s port=%u tls=%d topic=%s timeout_ms=%u\n",
        smoke->host,
        smoke->port,
        smoke->tls,
        topic,
        smoke->timeout_ms);

    h2_desktop_network_services_t *network = NULL;
    h2_pal_result_t rc =
        h2_desktop_network_services_create(1, 0, &network);
    if (rc != H2_PAL_OK) {
        fprintf(stderr, "PAL E2E network initialization failed: %d\n", rc);
        return 1;
    }
    h2_runtime_config_t runtime_config = make_runtime_config(network);
    h2_runtime_t *runtime = NULL;
    rc = h2_runtime_init(&runtime_config, &runtime);
    if (rc != H2_PAL_OK) {
        fprintf(stderr, "PAL E2E Runtime initialization failed: %d\n", rc);
        h2_desktop_network_services_destroy(network);
        return 1;
    }

    uint8_t network_buffer[4096];
    const h2_pal_e2e_config_t config = {
        .suite_mask = H2_PAL_E2E_SUITE_MQTT,
        .mqtt = {
            .host = smoke->host,
            .port = smoke->port,
            .transport = smoke->tls ? H2_PAL_MQTT_TRANSPORT_TLS : H2_PAL_MQTT_TRANSPORT_TCP,
            .client_id = { .data = client_id, .len = strlen(client_id) },
            .topic = { .data = topic, .len = strlen(topic) },
            .payload = { .data = (const uint8_t *)payload, .len = strlen(payload) },
            .timeout_ms = smoke->timeout_ms,
            .network_buffer = network_buffer,
            .network_buffer_len = sizeof(network_buffer),
        },
    };
    h2_pal_e2e_result_t result;
    rc = h2_pal_e2e_run(runtime, &config, &result);
    fprintf(
        stderr,
        "H2_PAL_E2E case=mqtt status=%s stage=%d rc=%d selected=%zu passed=%zu failed=%zu connected=%d suback=%d echo=%d disconnected=%d complete=%d\n",
        rc == H2_PAL_OK ? "PASS" : "FAIL",
        result.stage,
        rc,
        result.selected,
        result.passed,
        result.failed,
        result.connected_events,
        result.subscribe_ack_events,
        result.publish_echo_events,
        result.disconnected_events,
        result.complete);
    h2_runtime_deinit(runtime);
    h2_desktop_network_services_destroy(network);
    return rc == H2_PAL_OK ? 0 : 1;
}

int main(void) {
#if defined(H2_MQTT_SMOKE_LOOPBACK)
    loopback_broker_t broker;
    pthread_t thread;
    uint16_t port = start_loopback_broker(&broker, &thread);
    smoke_config_t smoke = {
        .host = "127.0.0.1",
        .port = port,
        .tls = 0,
        .topic_prefix = "h2/loopback-smoke",
        .timeout_ms = 2000u,
    };
    int status = run_smoke_with_config(&smoke);
    TEST_CHECK(pthread_join(thread, NULL) == 0);
    TEST_CHECK(pthread_mutex_destroy(&broker.lock) == 0);
    TEST_CHECK(pthread_cond_destroy(&broker.cond) == 0);
    return status == 0 && broker.status == 0 ? 0 : 1;
#else
    smoke_config_t smoke;
    if (load_smoke_config(&smoke) != 0) {
        return 1;
    }
    return run_smoke_with_config(&smoke);
#endif
}
