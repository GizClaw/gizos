#include "h2_pal_wifi_e2e.h"
#include "h2_web_main_thread.h"
#include "h2_web_application_host.h"
#include <emscripten/threading.h>
#include <stdio.h>

static int transition(h2_runtime_t *rt, h2_web_platform_t *platform, int online) {
    uint64_t start = 0, now = 0;
    (void)h2_pal_time_get_monotonic_ms(rt->time, &start);
    union {
        uint64_t align;
        unsigned char bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    } payload;
    h2_runtime_event_t e = {.payload = payload.bytes, .payload_capacity = sizeof(payload.bytes)};
    do {
        (void)h2_web_platform_pump(platform, 32u, NULL);
        while (h2_runtime_poll_event(rt, &e) == H2_PAL_OK) {
            if (e.kind != H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED)
                continue;
            const h2_runtime_system_event_netif_default_changed_t *v = e.payload;
            if (e.component != H2_RUNTIME_COMPONENT_SYSTEM_NETIF || e.payload_size != sizeof(*v) ||
                v->current_valid != (unsigned)online || v->previous_valid == (unsigned)online)
                return H2_PAL_ERR_IO;
            const h2_runtime_system_netif_ref_t *ref = online ? &v->current : &v->previous;
            if (!ref->name_valid || ref->kind != H2_RUNTIME_SYSTEM_NETIF_KIND_HOST)
                return H2_PAL_ERR_IO;
            h2_pal_netif_status_t status;
            int rc = h2_pal_netif_get_status(rt->netif, NULL, &status);
            if (online ? rc || !h2_pal_netif_status_is_usable(&status) : rc != H2_PAL_ERR_NOT_FOUND)
                return H2_PAL_ERR_IO;
            return H2_PAL_OK;
        }
        h2_web_worker_sleep(20);
        (void)h2_pal_time_get_monotonic_ms(rt->time, &now);
    } while (now - start < 10000);
    return H2_PAL_ERR_TIMEOUT;
}
static unsigned count;
static int offline, online;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *rt, void *user) {
    (void)user;
    h2_web_platform_t *platform = h2_web_application_host_platform(host);
    int rc = h2_wifi_host_contract(rt->wifi_sta, rt->wifi_ap, rt->wifi_settings, rt->netif,
                                   H2_PAL_NETIF_KIND_HOST, &count);
    if (!rc) {
        puts("H2_WIFI_WAIT_OFFLINE");
        fflush(stdout);
        rc = transition(rt, platform, 0);
        offline = !rc;
    }
    if (!rc) {
        puts("H2_WIFI_WAIT_ONLINE");
        fflush(stdout);
        rc = transition(rt, platform, 1);
        online = !rc;
    }
    return rc;
}
int main(void) {
    const h2_web_application_host_config_t config = {
        .name = "pal-wifi", .display_width = 1, .display_height = 1};
    const h2_application_host_config_t app = {.entry = run};
    h2_application_host_result_t lifecycle = {0};
    int rc = h2_web_application_host_run_app(&config, &app, &lifecycle);
    int teardown = lifecycle.cleanup;
    printf(
        "H2_WIFI_HOST_REPORT "
        "{\"platform\":\"wasm\",\"contract\":1,\"operations\":21,\"capability_contract_pass\":%d,"
        "\"physical_wifi_qualified\":0,\"wifi_sta\":\"UNSUPPORTED\",\"wifi_ap\":\"UNSUPPORTED\","
        "\"wifi_settings\":\"UNSUPPORTED\","
        "\"netif\":\"HOST\",\"netif_count\":%u,\"worker\":1,\"offline_event\":%d,\"online_event\":%"
        "d,\"rc\":%d,\"teardown\":%d}\n",
        !rc, count, offline, online, rc, teardown);
    return rc || teardown || lifecycle.retained ? 1 : 0;
}
