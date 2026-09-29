#include "h2_pal_wifi_e2e.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_platform.h"
#include "h2_web_main_thread.h"
#include <emscripten/threading.h>
#include <stdio.h>

static int transition(h2_runtime_t *rt, h2_web_platform_t *platform, int online) {
    uint64_t start = 0, now = 0; (void)h2_pal_time_get_monotonic_ms(rt->time, &start);
    union { uint64_t align; unsigned char bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX]; } payload;
    h2_runtime_event_t e = {.payload = payload.bytes, .payload_capacity = sizeof(payload.bytes)};
    do {
        (void)h2_web_platform_pump(platform, 32u, NULL);
        while (h2_runtime_poll_event(rt, &e) == H2_PAL_OK) {
            if (e.kind != H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED) continue;
            const h2_runtime_system_event_netif_default_changed_t *v = e.payload;
            if (e.component != H2_RUNTIME_COMPONENT_SYSTEM_NETIF || e.payload_size != sizeof(*v) ||
                v->current_valid != (unsigned)online || v->previous_valid == (unsigned)online) return H2_PAL_ERR_IO;
            const h2_runtime_system_netif_ref_t *ref = online ? &v->current : &v->previous;
            if (!ref->name_valid || ref->kind != H2_RUNTIME_SYSTEM_NETIF_KIND_HOST) return H2_PAL_ERR_IO;
            h2_pal_netif_status_t status;
            int rc = h2_pal_netif_get_status(rt->netif, NULL, &status);
            if (online ? rc || !h2_pal_netif_status_is_usable(&status) : rc != H2_PAL_ERR_NOT_FOUND) return H2_PAL_ERR_IO;
            return H2_PAL_OK;
        }
        h2_web_worker_sleep(20); (void)h2_pal_time_get_monotonic_ms(rt->time, &now);
    } while (now - start < 10000);
    return H2_PAL_ERR_TIMEOUT;
}
int main(void) {
    if (emscripten_is_main_runtime_thread()) return 2;
    const h2_web_platform_config_t pc = {.display_width = 1, .display_height = 1};
    h2_web_platform_t *platform = h2_web_platform_create(&pc); if (!platform) return 3;
    h2_runtime_config_t cfg = h2_smoke_host_runtime_config("pal-wifi", "web", "wasm32",
        h2_web_platform_mem_api(), h2_web_platform_time_api(platform), h2_web_platform_queue_api(platform), h2_pal_unsupported_display_api());
    cfg.netif = h2_web_platform_netif_api(platform); cfg.system_event = h2_web_platform_system_event_api(platform);
    h2_runtime_t *rt = NULL; unsigned count = 0;
    int rc = h2_runtime_init(&cfg, &rt);
    if (!rc) rc = h2_wifi_host_contract(rt->wifi_sta, rt->wifi_ap, rt->wifi_settings, rt->netif, H2_PAL_NETIF_KIND_HOST, &count);
    int offline = 0, online = 0;
    if (!rc) { puts("H2_WIFI_WAIT_OFFLINE"); fflush(stdout); rc = transition(rt, platform, 0); offline = !rc; }
    if (!rc) { puts("H2_WIFI_WAIT_ONLINE"); fflush(stdout); rc = transition(rt, platform, 1); online = !rc; }
    if (rt) h2_runtime_deinit(rt);
    int teardown = h2_web_platform_destroy(platform);
    printf("H2_WIFI_HOST_REPORT {\"platform\":\"wasm\",\"contract\":1,\"operations\":21,\"capability_contract_pass\":%d,"
        "\"physical_wifi_qualified\":0,\"wifi_sta\":\"UNSUPPORTED\",\"wifi_ap\":\"UNSUPPORTED\",\"wifi_settings\":\"UNSUPPORTED\","
        "\"netif\":\"HOST\",\"netif_count\":%u,\"worker\":1,\"offline_event\":%d,\"online_event\":%d,\"rc\":%d,\"teardown\":%d}\n", !rc, count, offline, online, rc, teardown);
    return rc || teardown ? 1 : 0;
}
