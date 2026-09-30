#include "h2/pal/h2_pal_unsupported.h"
#include "h2_darwin_platform.h"
#include "h2_pal_wifi_e2e.h"
#include <stdio.h>
int main(void) {
    unsigned count = 0;
    int rc = h2_wifi_host_contract(
        h2_pal_unsupported_wifi_sta_api(), h2_pal_unsupported_wifi_ap_api(),
        h2_pal_unsupported_wifi_settings_api(), h2_darwin_netif_api(), 0, &count);
    printf("H2_WIFI_HOST_REPORT "
           "{\"platform\":\"macos\",\"operations\":21,\"capability_contract_pass\":%d,"
           "\"physical_wifi_qualified\":0,\"wifi_sta\":\"UNSUPPORTED\",\"wifi_ap\":\"UNSUPPORTED\","
           "\"wifi_settings\":\"UNSUPPORTED\",\"netif\":\"read-only-real-interfaces\",\"netif_"
           "count\":%u,\"rc\":%d}\n",
           !rc, count, rc);
    return rc ? 1 : 0;
}
