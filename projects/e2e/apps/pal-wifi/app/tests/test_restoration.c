#include "h2_pal_wifi_e2e.h"
#include "h2/pal/h2_pal_unsupported.h"
#include <assert.h>
#include <string.h>

typedef struct fake { h2_pal_wifi_sta_config_t saved, original; uint64_t now; int has, linked, fail_restore; unsigned recorded, restore_calls; } fake_t;
static int has(void *u, int *out) { *out = ((fake_t *)u)->has; return 0; }
static int get(void *u, h2_pal_wifi_sta_config_t *out) { fake_t *f = u; if (!f->has) return H2_PAL_ERR_NOT_FOUND; *out = f->saved; return 0; }
static int set(void *u, const h2_pal_wifi_sta_config_t *c) { fake_t *f = u;
    if (h2_wifi_config_equal(c, &f->original)) { ++f->restore_calls; if (f->fail_restore) return H2_PAL_ERR_IO; }
    f->saved = *c; f->has = 1; return 0;
}
static int clear(void *u) { ((fake_t *)u)->has = 0; return 0; }
static int ap_status(void *u, h2_pal_wifi_ap_status_t *out) { (void)u; memset(out,0,sizeof(*out)); out->state = H2_PAL_WIFI_AP_STATE_STOPPED; return 0; }
static int ap_stop(void *u, uint32_t timeout) { (void)u; (void)timeout; return 0; }
static int disconnected(void *u) { ((fake_t *)u)->linked = 0; return 0; }
static int connected(void *u, const h2_pal_wifi_sta_config_t *c, uint32_t timeout) {
    (void)timeout; fake_t *f = u; if (!h2_wifi_config_equal(c,&f->original)) return H2_PAL_ERR_UNSUPPORTED;
    f->linked = 1; return 0;
}
static int sta_status(void *u, h2_pal_wifi_sta_status_t *out) {
    fake_t *f = u; memset(out,0,sizeof(*out)); out->state = f->linked ? H2_PAL_WIFI_STA_STATE_GOT_IP : H2_PAL_WIFI_STA_STATE_DISCONNECTED;
    if (f->linked) { out->ssid_len=f->original.ssid_len; memcpy(out->ssid,f->original.ssid,out->ssid_len); out->ip_valid=1; out->ip.ip4=0x01020304; }
    return 0;
}
static int power(void *u, h2_pal_wifi_power_save_t mode) { (void)u; (void)mode; return 0; }
static int time(void *u, uint64_t *out) { *out=++((fake_t *)u)->now; return 0; }
static int sleep(void *u,uint32_t ms) { ((fake_t *)u)->now += ms; return 0; }
static void record(void *u,const char *id,int rc,uint64_t elapsed) { (void)id;(void)rc;(void)elapsed;++((fake_t *)u)->recorded; }
static void test(int fail_restore) {
    fake_t f={.original={.ssid="original",.ssid_len=8},.has=1,.fail_restore=fail_restore};f.saved=f.original;
    h2_pal_wifi_settings_vtable_t settings_v={get,set,clear,has};h2_pal_wifi_settings_api_t settings={&f,&settings_v};
    h2_pal_wifi_sta_vtable_t sta_v={.get_status=sta_status,.connect=connected,.disconnect=disconnected,.set_power_save=power};h2_pal_wifi_sta_api_t sta={&f,&sta_v};
    h2_pal_wifi_ap_vtable_t ap_v={.get_status=ap_status,.stop=ap_stop};h2_pal_wifi_ap_api_t ap={&f,&ap_v};
    h2_pal_time_vtable_t time_v={.get_monotonic_ms=time,.sleep_ms=sleep};h2_pal_time_api_t time_api={&f,&time_v};
    h2_runtime_t rt={.time=&time_api,.wifi_sta=&sta,.wifi_ap=&ap,.wifi_settings=&settings,.netif=h2_pal_unsupported_netif_api()};
    h2_wifi_e2e_config_t cfg={.fixture={.ssid="fixture",.ssid_len=7,.password="synthetic",.password_len=9},
        .ap={.ssid="dut",.ssid_len=3,.security=H2_PAL_WIFI_SECURITY_OPEN},.operation_timeout_ms=50,.client_timeout_ms=50,.case_result=record,.user=&f};
    h2_wifi_e2e_result_t r={0};assert(h2_wifi_e2e_run(&rt,&cfg,&r)!=0);
    assert(f.recorded==38 && f.restore_calls==1);assert(r.passed+r.failed+r.blocked==38);
    if (!fail_restore) { assert(!r.cleanup_rc && !r.retained && r.saved_restored && r.network_restored); assert(h2_wifi_config_equal(&f.saved,&f.original)); }
    else { assert(r.cleanup_rc==H2_PAL_ERR_IO && r.retained==1 && !r.saved_restored); }
}
int main(void) { test(0);test(1);return 0; }
