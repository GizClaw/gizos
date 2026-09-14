#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "h2/pal/net/h2_pal_netif.h"
#include "h2/pal/hal/h2_pal_wifi.h"
typedef uint8_t u8_t;
typedef struct { uint32_t addr; } ip_addr_t;
#define IP_IS_V4(p) ((p) != NULL)
#define ip_addr_isany(p) ((p)->addr == 0)
#define ip_2_ip4(p) (p)
#define DNS_MAX_SERVERS 2
#define WIFI_NETIF 1
#define AP_MODE 2
#define ERR_OK 0
struct wifi_mode_info { int mode; };
enum wifi_sta_connect_state { WIFI_STA_CONNECT_SUCC, WIFI_STA_NETWORK_STACK_DHCP_SUCC, WIFI_STA_NETWORK_STACK_DHCP_TIMEOUT };
struct lan_setting {
    uint8_t WIRELESS_IP_ADDR0, WIRELESS_IP_ADDR1, WIRELESS_IP_ADDR2, WIRELESS_IP_ADDR3;
    uint8_t WIRELESS_NETMASK0, WIRELESS_NETMASK1, WIRELESS_NETMASK2, WIRELESS_NETMASK3;
    uint8_t WIRELESS_GATEWAY0, WIRELESS_GATEWAY1, WIRELESS_GATEWAY2, WIRELESS_GATEWAY3;
};
struct netif_info { uint32_t ip, gw, netmask; };
static _Thread_local int tcpip_owner;
static atomic_int radio_pin;
static int offline, dispatch_error, generation_error;
void wifi_get_mode_cur_info(struct wifi_mode_info *mode) { mode->mode = 1; }
int wifi_is_on(void) { return !offline; }
enum wifi_sta_connect_state wifi_get_sta_connect_state(void) { return WIFI_STA_NETWORK_STACK_DHCP_SUCC; }
void wifi_get_bssid(uint8_t *mac) { memset(mac, 2, 6); }
int wifi_get_channel(void) { return 6; }
int wifi_get_rssi(void) { return -42; }
int wifi_get_mac(uint8_t *mac) { memset(mac, 7, 6); return 0; }
struct lan_setting *net_get_lan_info(int id) {
    (void)id;
    assert(tcpip_owner);
    static struct lan_setting lan = {1, 2, 3, 4, 255, 255, 255, 0, 1, 2, 3, 1};
    return &lan;
}
void lwip_get_netif_info(u8_t id, struct netif_info *info) {
    assert(id == WIFI_NETIF && tcpip_owner && atomic_load(&radio_pin));
    const uint8_t ip[4] = {1, 2, 3, 4};
    const uint8_t mask[4] = {255, 255, 255, 0};
    const uint8_t gateway[4] = {1, 2, 3, 1};
    memcpy(&info->ip, ip, 4);
    memcpy(&info->netmask, mask, 4);
    memcpy(&info->gw, gateway, 4);
}
const ip_addr_t *dns_getserver(u8_t index) {
    assert(tcpip_owner && atomic_load(&radio_pin));
    static const ip_addr_t servers[2] = {{0x08080808}, {0}};
    return &servers[index];
}
const char *os_current_task(void) { return tcpip_owner ? "tcpip_thread" : "reader"; }
struct dispatch { void (*fn)(void *); void *arg; };
static void *dispatch_worker(void *arg) {
    struct dispatch *dispatch = arg;
    tcpip_owner = 1;
    dispatch->fn(dispatch->arg);
    return NULL;
}
int tcpip_callback_wait(void (*fn)(void *), void *arg) {
    assert(!tcpip_owner && atomic_load(&radio_pin));
    if (dispatch_error) return -1;
    struct dispatch dispatch = {fn, arg};
    pthread_t worker;
    assert(pthread_create(&worker, NULL, dispatch_worker, &dispatch) == 0);
    assert(pthread_join(worker, NULL) == 0);
    return 0;
}
int h2_jieli_wifi_netif_begin(h2_pal_netif_status_t *status, uint32_t *generation) {
    assert(atomic_exchange(&radio_pin, 1) == 0);
    memset(status, 0, sizeof(*status));
    status->ref.type = H2_PAL_NETIF_REF_NAME;
    status->ref.kind = status->kind = H2_PAL_NETIF_KIND_WIFI_STA;
    strcpy(status->ref.name, "wl0");
    status->mtu = 1500;
    if (!offline) status->flags = H2_PAL_NETIF_FLAG_UP | H2_PAL_NETIF_FLAG_LINK_UP | H2_PAL_NETIF_FLAG_HAS_IPV4;
    *generation = 1;
    return H2_PAL_OK;
}
int h2_jieli_wifi_netif_end(uint32_t generation) {
    assert(generation == 1 && atomic_exchange(&radio_pin, 0) == 1);
    return generation_error ? H2_PAL_ERR_BUSY : H2_PAL_OK;
}
/* REAL_PROVIDER */
static h2_pal_result_t subscriber(void *user, const h2_pal_netif_ref_t *ref,
    const h2_pal_netif_status_t *status) {
    (void)user;
    assert(!atomic_load(&radio_pin));
    assert(strcmp(ref->name, "wl0") == 0 && status->dns_count == 1);
    return H2_PAL_ERR_IO;
}
int main(void) {
    h2_pal_wifi_sta_status_t sta = {.state = H2_PAL_WIFI_STA_STATE_GOT_IP};
    atomic_store(&radio_pin, 1);
    update_sta_snapshot(&sta);
    assert(sta.ip_valid && sta.ip.ip4 == 0x01020304);
    dispatch_error = 1;
    sta.ip_valid = 0;
    update_sta_snapshot(&sta);
    assert(!sta.ip_valid);
    dispatch_error = 0;
    atomic_store(&radio_pin, 0);
    h2_pal_netif_status_t status;
    assert(status_for_wifi(&status) == H2_PAL_OK);
    assert(status.ipv4.ip[0] == 1 && status.ipv4.ip[3] == 4);
    assert(status.dns_count == 1 && status.dns[0].addr.ip[0] == 8);
    assert(!atomic_load(&radio_pin));
    assert(h2_jieli_netif_list(NULL, NULL, subscriber, NULL) == H2_PAL_ERR_IO);
    tcpip_owner = 1;
    assert(status_for_wifi(&status) == H2_PAL_OK);
    tcpip_owner = 0;
    dispatch_error = 1;
    assert(status_for_wifi(&status) == H2_PAL_ERR_IO);
    assert(!atomic_load(&radio_pin));
    dispatch_error = 0;
    generation_error = 1;
    assert(status_for_wifi(&status) == H2_PAL_ERR_BUSY);
    assert(!atomic_load(&radio_pin));
    generation_error = 0;
    offline = 1;
    assert(status_for_wifi(&status) == H2_PAL_OK && status.flags == 0);
    assert(status.dns_count == 0 && !atomic_load(&radio_pin));
    assert(h2_jieli_netif_get_dns(NULL, NULL, NULL, 0, &(size_t){0}) == H2_PAL_OK);
    return 0;
}
