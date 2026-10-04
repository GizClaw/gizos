#include "h2_pal_mqtt_e2e.h"
#include "h2_coremqtt.h"
#include "h2_desktop_platform.h"
#include "h2_wolfssl.h"
#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#define host_net h2_darwin_net_api
#define host_entropy h2_darwin_entropy
#else
#include "h2_linux_platform.h"
#define host_net h2_linux_net_api
#define host_entropy h2_linux_entropy
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct allocations {
    const h2_pal_mem_api_t *backing;
    void *slots[128];
    unsigned live;
    unsigned invalid;
} allocations_t;

static void *tracked_realloc(void *user, void *pointer, size_t length) {
    allocations_t *tracker = user;
    for (unsigned index = 0u; index < 128u; ++index) {
        if (tracker->slots[index] == pointer) {
            void *next = h2_pal_mem_realloc(tracker->backing, pointer, length == 0u ? 1u : length);
            if (next != NULL) {
                if (pointer == NULL) ++tracker->live;
                tracker->slots[index] = next;
            }
            return next;
        }
    }
    ++tracker->invalid;
    return NULL;
}

static void *tracked_alloc(void *user, size_t length) { return tracked_realloc(user, NULL, length); }

static void tracked_free(void *user, void *pointer) {
    allocations_t *tracker = user;
    if (pointer == NULL) return;
    for (unsigned index = 0u; index < 128u; ++index) {
        if (tracker->slots[index] == pointer) {
            tracker->slots[index] = NULL; --tracker->live;
            h2_pal_mem_free(tracker->backing, pointer);
            return;
        }
    }
    ++tracker->invalid;
}

static const h2_pal_mem_vtable_t tracked_vtable = {.alloc = tracked_alloc, .realloc = tracked_realloc, .free = tracked_free};

static void report(void *user, const h2_pal_mqtt_e2e_case_result_t *result) {
    allocations_t *tracker = user;
    printf("H2_PAL_MQTT_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u,\"elapsed_ms\":%llu,\"connected\":%u,\"received\":%u,\"disconnected\":%u,\"live_allocations\":%u,\"invalid_frees\":%u}\n",
        result->id, result->passed ? "PASS" : result->blocked ? "BLOCKED" : "FAIL",
        result->detail, result->line, (unsigned long long)result->elapsed_ms,
        result->connected, result->received, result->disconnected, tracker->live, tracker->invalid);
    fflush(stdout);
}

static int read_pem(const char *path, uint8_t *buffer, size_t capacity, size_t *out_length) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    *out_length = fread(buffer, 1u, capacity, file);
    int valid = !ferror(file) && feof(file) && *out_length != 0u;
    fclose(file);
    return valid;
}

static int number(const char *text, unsigned maximum, unsigned *out) {
    if (text == NULL || text[0] < '0' || text[0] > '9') return 0;
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end != '\0' || value == 0u || value > maximum) return 0;
    *out = (unsigned)value;
    return 1;
}

#if defined(H2_MQTT_PUBLIC_BROKER)
static int parse_bool(const char *text, int *out) {
    char normalized[8];
    if (text == NULL || text[0] == '\0') { *out = 0; return 1; }
    size_t length = strlen(text);
    if (length >= sizeof(normalized)) return 0;
    for (size_t index = 0u; index <= length; ++index) {
        char value = text[index];
        normalized[index] = value >= 'A' && value <= 'Z' ? (char)(value + 'a' - 'A') : value;
    }
    if (strcmp(normalized, "1") == 0 || strcmp(normalized, "true") == 0 ||
        strcmp(normalized, "yes") == 0 || strcmp(normalized, "on") == 0) { *out = 1; return 1; }
    if (strcmp(normalized, "0") == 0 || strcmp(normalized, "false") == 0 ||
        strcmp(normalized, "no") == 0 || strcmp(normalized, "off") == 0) { *out = 0; return 1; }
    return 0;
}
#endif

int main(int argc, char **argv) {
    allocations_t tracker = {.backing = h2_desktop_platform_default_allocator()};
    h2_pal_mem_api_t memory = {.user = &tracker, .vtable = &tracked_vtable};
    h2_runtime_t runtime = {.mem = &memory, .net = host_net(), .time = h2_desktop_platform_time_api()};
    uint8_t ca[8192], wrong_ca[8192];
    size_t ca_len = 0u, wrong_ca_len = 0u;
    h2_pal_net_tls_config_t trusted = {.server_name = "localhost", .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED};
    h2_pal_net_tls_config_t untrusted = trusted, wrong_name = trusted;
    h2_pal_mqtt_e2e_config_t config = {.runtime = &runtime, .timeout_ms = 3000u,
        .qos_publish_capacity = 4u,
        .trusted_tls = &trusted, .untrusted_tls = &untrusted, .wrong_name_tls = &wrong_name,
        .report = report, .report_user = &tracker};
#if defined(H2_MQTT_PUBLIC_BROKER)
    (void)argv;
    if (argc != 1) return 2;
    config.smoke_only = 1;
    config.host = getenv("H2_MQTT_SMOKE_HOST");
    if (config.host == NULL || config.host[0] == '\0') config.host = "broker.emqx.io";
    config.topic_prefix = getenv("H2_MQTT_SMOKE_TOPIC_PREFIX");
    if (config.topic_prefix == NULL || config.topic_prefix[0] == '\0') config.topic_prefix = "h2/public-smoke";
    const char *tls_env = getenv("H2_MQTT_SMOKE_TLS");
    int tls_enabled = 0;
    if (!parse_bool(tls_env, &tls_enabled)) return 2;
    unsigned port = tls_enabled ? 8883u : 1883u;
    const char *port_env = getenv("H2_MQTT_SMOKE_PORT");
    if (port_env != NULL && port_env[0] != '\0' && !number(port_env, 65535u, &port)) return 2;
    unsigned timeout = 5000u;
    const char *timeout_env = getenv("H2_MQTT_SMOKE_TIMEOUT_MS");
    if (timeout_env != NULL && timeout_env[0] != '\0' && !number(timeout_env, 60000u, &timeout)) return 2;
    config.timeout_ms = timeout; config.tcp_port = (uint16_t)port; config.tls_port = (uint16_t)port;
    config.smoke_transport = tls_enabled ? H2_PAL_MQTT_TRANSPORT_TLS : H2_PAL_MQTT_TRANSPORT_TCP;
    trusted.server_name = config.host;
    const char *ca_file = getenv("H2_MQTT_SMOKE_CA_FILE");
    if (ca_file != NULL && ca_file[0] != '\0' && !read_pem(ca_file, ca, sizeof(ca), &ca_len)) return 2;
    char session[64];
    uint8_t nonce[16];
    if (host_entropy(NULL, nonce, sizeof(nonce)) != H2_PAL_OK) return 2;
    for (size_t index = 0u; index < sizeof(nonce); ++index) (void)snprintf(session + index * 2u, 3u, "%02x", nonce[index]);
    config.session = session;
#else
    if (argc != 6) return 2;
    unsigned tcp_port, tls_port;
    if (!number(argv[1], 65535u, &tcp_port) || !number(argv[2], 65535u, &tls_port) ||
        !read_pem(argv[4], ca, sizeof(ca), &ca_len) || !read_pem(argv[5], wrong_ca, sizeof(wrong_ca), &wrong_ca_len)) return 2;
    config.host = "127.0.0.1"; config.tcp_port = (uint16_t)tcp_port; config.tls_port = (uint16_t)tls_port;
    config.session = argv[3]; config.topic_prefix = "h2/mqtt/e2e";
    wrong_name.server_name = "wrong-name.invalid";
#endif
    trusted.root_ca_pem = ca_len == 0u ? NULL : ca; trusted.root_ca_pem_len = ca_len;
    untrusted.root_ca_pem = wrong_ca; untrusted.root_ca_pem_len = wrong_ca_len;
    wrong_name.root_ca_pem = trusted.root_ca_pem; wrong_name.root_ca_pem_len = ca_len;
    h2_wolfssl_config_t tls = {.mem = memory, .entropy = host_entropy};
    if (h2_wolfssl_init(&tls) != H2_PAL_OK) return 2;
    h2_coremqtt_t *provider = NULL;
    h2_pal_mqtt_api_t api;
    h2_coremqtt_config_t provider_config = {.allocator = &memory, .net = runtime.net, .time = runtime.time,
        .outgoing_publish_records = 4u, .incoming_publish_records = 4u};
    if (h2_coremqtt_create(&provider_config, &provider, &api) != H2_PAL_OK) {
        (void)h2_wolfssl_deinit(); return 2;
    }
    runtime.mqtt = &api;
    h2_pal_mqtt_e2e_result_t result;
    int rc = h2_pal_mqtt_e2e_run(&config, &result);
    h2_coremqtt_destroy(provider);
    int cleanup = h2_wolfssl_deinit();
    printf("H2_PAL_MQTT_SUMMARY {\"selected\":%u,\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"retained_allocations\":%u,\"invalid_frees\":%u,\"cleanup\":%d}\n",
        result.selected, result.passed, result.failed, result.blocked, tracker.live, tracker.invalid, cleanup);
    return rc == H2_PAL_OK && cleanup == H2_PAL_OK && tracker.live == 0u && tracker.invalid == 0u ? 0 : 1;
}
