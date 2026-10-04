#include "mobile_runner.h"
#if defined(__APPLE__)
#include "h2_ios_mqtt.h"
#define mqtt_owner h2_ios_mqtt_t
#define mqtt_create h2_ios_mqtt_create
#define mqtt_api h2_ios_mqtt_api
#define mqtt_destroy h2_ios_mqtt_destroy
#define native_stats h2_ios_platform_resource_stats_t
#define read_stats h2_ios_platform_get_resource_stats
#else
#include "h2_android_mqtt.h"
#define mqtt_owner h2_android_mqtt_t
#define mqtt_create h2_android_mqtt_create
#define mqtt_api h2_android_mqtt_api
#define mqtt_destroy h2_android_mqtt_destroy
#define native_stats h2_android_platform_resource_stats_t
#define read_stats h2_android_platform_get_resource_stats
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct tracker {
    const h2_pal_mem_api_t *backing;
    void *slots[128];
    size_t live;
    unsigned invalid, attempts, reports;
    int fail_after;
    h2_mqtt_mobile_result_t *result;
} tracker_t;
static void *allocate_again(void *user, void *pointer, size_t length) {
    tracker_t *tracker = user;
    if (tracker->fail_after >= 0 && tracker->attempts++ >= (unsigned)tracker->fail_after) return NULL;
    for (unsigned index = 0u; index < 128u; ++index) {
        if (tracker->slots[index] == pointer) {
            void *next = h2_pal_mem_realloc(tracker->backing, pointer, length == 0u ? 1u : length);
            if (next != NULL) { if (pointer == NULL) ++tracker->live; tracker->slots[index] = next; }
            return next;
        }
    }
    ++tracker->invalid; return NULL;
}
static void *allocate(void *user, size_t length) { return allocate_again(user, NULL, length); }
static void release(void *user, void *pointer) {
    tracker_t *tracker = user;
    if (pointer == NULL) return;
    for (unsigned index = 0u; index < 128u; ++index) {
        if (tracker->slots[index] == pointer) {
            tracker->slots[index] = NULL; --tracker->live;
            h2_pal_mem_free(tracker->backing, pointer); return;
        }
    }
    ++tracker->invalid;
}
static const h2_pal_mem_vtable_t tracked_vtable = {.alloc = allocate, .realloc = allocate_again, .free = release};
static int snapshot(size_t out[9]) {
    native_stats stats = {0};
    int rc = read_stats(&stats);
    if (rc == H2_PAL_OK) {
        size_t values[] = {stats.tasks, stats.task_stack_bytes, stats.queues, stats.mutexes,
            stats.semaphores, stats.conditions, stats.timers, stats.allocations, stats.allocation_bytes};
        memcpy(out, values, sizeof(values));
    }
    return rc;
}
static void observe(void *user, const h2_pal_mqtt_e2e_case_result_t *item) {
    (void)item;
    tracker_t *tracker = user;
    if (tracker->reports < H2_PAL_MQTT_E2E_CASE_COUNT)
        tracker->result->case_allocations[tracker->reports++] = tracker->live;
    else ++tracker->invalid;
}
int h2_mqtt_mobile_run(h2_runtime_config_t config, const char *host,
    uint16_t tcp_port, uint16_t tls_port, const char *session,
    const uint8_t *ca, size_t ca_len, const uint8_t *wrong_ca, size_t wrong_ca_len,
    h2_mqtt_mobile_result_t *out) {
    if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (host == NULL || session == NULL || ca == NULL || ca_len == 0u ||
        wrong_ca == NULL || wrong_ca_len == 0u) return H2_PAL_ERR_INVALID_ARG;
    tracker_t tracker = {.backing = config.mem, .fail_after = -1, .result = out};
    h2_pal_mem_api_t memory = {.user = &tracker, .vtable = &tracked_vtable};
    int rc = snapshot(out->before);
    mqtt_owner *owner = NULL;
    /* Observe both partial initialization failures in the actual packaged SDK. */
    for (int failure = 0; rc == H2_PAL_OK && failure < 2; ++failure) {
        tracker.fail_after = failure; tracker.attempts = 0u;
        owner = (mqtt_owner *)(uintptr_t)1u;
        int observed = mqtt_create(&memory, &owner);
        if (observed != H2_PAL_ERR_NO_MEMORY || owner != NULL || tracker.live != 0u || tracker.invalid != 0u)
            rc = H2_PAL_ERR_IO;
        else ++out->owner_failure_verified;
    }
    tracker.fail_after = -1;
    if (rc == H2_PAL_OK) rc = mqtt_create(&memory, &owner);
    h2_runtime_t *runtime = NULL;
    if (rc == H2_PAL_OK) {
        config.mem = &memory; config.mqtt = mqtt_api(owner);
        rc = h2_runtime_init(&config, &runtime);
    }
    if (rc == H2_PAL_OK) {
        h2_pal_net_tls_config_t trusted = {.server_name = "localhost", .root_ca_pem = ca,
            .root_ca_pem_len = ca_len, .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED};
        h2_pal_net_tls_config_t untrusted = trusted, wrong_name = trusted;
        untrusted.root_ca_pem = wrong_ca; untrusted.root_ca_pem_len = wrong_ca_len;
        wrong_name.server_name = "wrong-name.invalid";
        h2_pal_mqtt_e2e_config_t fixture = {.runtime = runtime, .host = host,
            .tcp_port = tcp_port, .tls_port = tls_port, .session = session,
            .topic_prefix = "h2/mqtt/e2e", .trusted_tls = &trusted,
            .untrusted_tls = &untrusted, .wrong_name_tls = &wrong_name,
            .timeout_ms = 5000u, .qos_publish_capacity = 4u, .report = observe, .report_user = &tracker};
        rc = h2_pal_mqtt_e2e_run(&fixture, &out->suite);
    }
    if (runtime != NULL) h2_runtime_deinit(runtime);
    out->owner_destroy = mqtt_destroy(&owner);
    int after = snapshot(out->after);
    out->retained_allocations = tracker.live; out->invalid_frees = tracker.invalid;
    for (unsigned index = 0u; index < tracker.reports; ++index)
        if (out->case_allocations[index] != out->case_allocations[0]) rc = H2_PAL_ERR_IO;
    if (out->owner_destroy != H2_PAL_OK || after != H2_PAL_OK ||
        memcmp(out->before, out->after, sizeof(out->before)) != 0 || tracker.live != 0u || tracker.invalid != 0u)
        rc = H2_PAL_ERR_IO;
    return rc;
}
int h2_mqtt_mobile_report(const char *path, const char *platform, const char *version,
    const h2_mqtt_mobile_result_t *result, int rc, int teardown) {
    FILE *file = fopen(path, "w"); if (file == NULL) return H2_PAL_ERR_IO;
    fprintf(file, "{\"platform\":\"%s\",\"version\":\"%s\",\"pid\":%ld,\"contract\":1,\"operations\":8,\"selected\":%u,\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"retained_allocations\":%zu,\"invalid_frees\":%u,\"owner_failure_verified\":%u,\"owner_destroy\":%d,\"rc\":%d,\"teardown\":%d,\"before\":[",
        platform, version, (long)getpid(), result->suite.selected, result->suite.passed,
        result->suite.failed, result->suite.blocked, result->retained_allocations,
        result->invalid_frees, result->owner_failure_verified, result->owner_destroy, rc, teardown);
    for (unsigned i = 0u; i < 9u; ++i) fprintf(file, "%s%zu", i ? "," : "", result->before[i]);
    fputs("],\"after\":[", file);
    for (unsigned i = 0u; i < 9u; ++i) fprintf(file, "%s%zu", i ? "," : "", result->after[i]);
    fputs("],\"cases\":[", file);
    for (unsigned i = 0u; i < H2_PAL_MQTT_E2E_CASE_COUNT; ++i) {
        const h2_pal_mqtt_e2e_case_result_t *item = &result->suite.cases[i];
        fprintf(file, "%s{\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u,\"elapsed_ms\":%llu,\"live_allocations\":%zu,\"connected\":%u,\"received\":%u,\"disconnected\":%u}",
            i ? "," : "", item->id ? item->id : "", item->passed ? "PASS" : item->blocked ? "BLOCKED" : "FAIL",
            item->detail, item->line, (unsigned long long)item->elapsed_ms, result->case_allocations[i],
            item->connected, item->received, item->disconnected);
    }
    fputs("]}\n", file);
    int valid = !ferror(file); if (fclose(file) != 0) valid = 0;
    return valid && rc == H2_PAL_OK && teardown == H2_PAL_OK &&
        result->suite.passed == H2_PAL_MQTT_E2E_CASE_COUNT ? H2_PAL_OK : H2_PAL_ERR_IO;
}
