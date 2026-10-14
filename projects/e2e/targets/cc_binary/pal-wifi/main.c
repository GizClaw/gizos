#include "h2/pal/h2_pal_unsupported.h"
#include "h2_desktop_application_host.h"
#include "h2_pal_wifi_e2e.h"
#include <stdio.h>
static unsigned count;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
    (void)host; (void)user;
    return h2_wifi_host_contract(runtime->wifi_sta, runtime->wifi_ap,
        runtime->wifi_settings, runtime->netif, 0, &count);
}
int main(void) {
    const h2_desktop_application_host_config_t config = {0};
    const h2_application_host_config_t app = {.entry = run};
    h2_application_host_result_t lifecycle = {0};
    int rc = h2_desktop_application_host_run(&config, &app, &lifecycle);
    printf("H2_WIFI_HOST_REPORT "
           "{\"platform\":\"macos\",\"operations\":21,\"capability_contract_pass\":%d,"
           "\"physical_wifi_qualified\":0,\"wifi_sta\":\"UNSUPPORTED\",\"wifi_ap\":\"UNSUPPORTED\","
           "\"wifi_settings\":\"UNSUPPORTED\",\"netif\":\"read-only-real-interfaces\",\"netif_"
           "count\":%u,\"rc\":%d}\n",
           !rc, count, rc);
    return rc ? 1 : 0;
}
