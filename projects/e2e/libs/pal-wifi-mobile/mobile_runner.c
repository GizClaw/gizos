#include "mobile_runner.h"
#include "h2_pal_wifi_e2e.h"
#include <stdio.h>
#include <unistd.h>

int h2_wifi_mobile_run(h2_runtime_config_t config, const char *platform,
                         const char *version, const char *path,
                         int (*shutdown)(void)) {
    h2_runtime_t *runtime = NULL;
    int rc = h2_runtime_init(&config, &runtime);
    unsigned count = 0;
    if (!rc) rc = h2_wifi_host_contract(runtime->wifi_sta, runtime->wifi_ap,
                                        runtime->wifi_settings, runtime->netif, -1, &count);
    if (runtime) h2_runtime_deinit(runtime);
    int teardown = shutdown();
    FILE *f = fopen(path, "w"); if (!f) return H2_PAL_ERR_IO;
    fprintf(f, "{\"platform\":\"%s\",\"version\":\"%s\",\"pid\":%ld,\"contract\":1,"
               "\"operations\":21,\"capability_contract_pass\":%d,\"physical_wifi_qualified\":0,"
               "\"netif_available\":0,\"wifi_sta\":\"UNSUPPORTED\",\"wifi_ap\":\"UNSUPPORTED\","
               "\"wifi_settings\":\"UNSUPPORTED\",\"netif\":\"UNSUPPORTED\",\"rc\":%d,\"teardown\":%d}\n",
            platform, version, (long)getpid(), rc == H2_PAL_OK, rc, teardown);
    if (fclose(f)) return H2_PAL_ERR_IO;
    return rc ? rc : teardown;
}
