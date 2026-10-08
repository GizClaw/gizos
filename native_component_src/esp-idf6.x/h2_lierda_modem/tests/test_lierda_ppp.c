#include "h2_esp_lierda_modem.h"
#include "lierda_test_sdk.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_mutex { bool locked; };
struct sdk_events { EventBits_t bits; };
struct sdk_handler {
    esp_event_base_t base;
    int32_t id;
    esp_event_handler_t callback;
    void *user;
    bool active;
};

static struct {
    struct sdk_handler handlers[3];
    esp_netif_t *netif;
    unsigned allocations, groups, netifs, dces, commands;
    unsigned power_on, power_off, unregisters;
    unsigned registry_entries;
    unsigned reconciles;
    bool silent_data, silent_stop, command_mode, fail_command_mode, fail_power_off, fail_unregister;
    bool fail_ip_clear, long_response;
    esp_modem_dte_config_t dte;
    char dial_apn[64];
} sdk;

static void event(esp_event_base_t base, int32_t id, void *data) {
    for (size_t i = 0u; i < 3u; ++i) {
        struct sdk_handler *h = &sdk.handlers[i];
        if (h->active && h->base == base && (h->id == id || h->id == ESP_EVENT_ANY_ID)) {
            h->callback(h->user, base, id, data);
        }
    }
}

static void got_ip(uint32_t addr, bool current) {
    if (current) sdk.netif->ip.ip.addr = addr;
    const ip_event_got_ip_t payload = {.esp_netif = sdk.netif, .ip_info = {.ip = {.addr = addr}}};
    event(IP_EVENT, IP_EVENT_PPP_GOT_IP, (void *)&payload);
}

EventGroupHandle_t xEventGroupCreate(void) {
    ++sdk.groups;
    return calloc(1u, sizeof(struct sdk_events));
}
void vEventGroupDelete(EventGroupHandle_t group) {
    for (size_t i = 0u; i < 3u; ++i) assert(!sdk.handlers[i].active);
    --sdk.groups;
    free(group);
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) {
    group->bits |= bits;
    return group->bits;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
    const EventBits_t before = group->bits;
    group->bits &= ~bits;
    return before;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
    int clear, int all, uint32_t timeout) {
    (void)timeout;
    assert(!clear && !all);
    return group->bits & bits;
}

esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    esp_event_handler_t callback, void *user, esp_event_handler_instance_t *out) {
    for (size_t i = 0u; i < 3u; ++i) {
        if (!sdk.handlers[i].active) {
            sdk.handlers[i] = (struct sdk_handler){base, id, callback, user, true};
            *out = &sdk.handlers[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
    esp_event_handler_instance_t instance) {
    assert(instance->active && instance->base == base && instance->id == id);
    if (sdk.fail_unregister) return ESP_FAIL;
    ++sdk.unregisters;
    instance->active = false;
    return ESP_OK;
}

esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_netif_t *esp_netif_new(const esp_netif_config_t *config) {
    (void)config;
    ++sdk.netifs;
    sdk.netif = calloc(1u, sizeof(*sdk.netif));
    return sdk.netif;
}
void esp_netif_destroy(esp_netif_t *netif) {
    assert(sdk.dces == 0u && sdk.registry_entries == 0u);
    for (size_t i = 0u; i < 3u; ++i) assert(!sdk.handlers[i].active);
    --sdk.netifs;
    free(netif);
    sdk.netif = NULL;
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out) {
    *out = netif->ip;
    return ESP_OK;
}
esp_err_t esp_netif_set_ip_info(esp_netif_t *netif, const esp_netif_ip_info_t *ip) {
    if (sdk.fail_ip_clear) return ESP_FAIL;
    netif->ip = *ip;
    return ESP_OK;
}
esp_err_t esp_netif_get_dns_info(esp_netif_t *netif, int type, esp_netif_dns_info_t *out) {
    (void)netif;
    out->ip.type = ESP_IPADDR_TYPE_V4;
    out->ip.u_addr.ip4.addr = type == ESP_NETIF_DNS_MAIN ? 1u : 2u;
    return ESP_OK;
}
esp_err_t esp_netif_ppp_set_params(esp_netif_t *netif, const esp_netif_ppp_config_t *config) {
    (void)netif;
    assert(config->ppp_phase_event_enabled && config->ppp_error_event_enabled);
    return ESP_OK;
}
esp_err_t esp_netif_ppp_set_auth(esp_netif_t *netif, esp_netif_auth_type_t type,
    const char *username, const char *password) {
    (void)netif;
    if (username[0] == '\0' && password[0] == '\0') assert(type == NETIF_PPP_AUTHTYPE_NONE);
    else assert(type != NETIF_PPP_AUTHTYPE_NONE);
    return ESP_OK;
}
void h2_esp_platform_netif_register(esp_netif_t *netif, h2_pal_netif_kind_t kind) {
    assert(netif == sdk.netif && kind == H2_PAL_NETIF_KIND_MODEM_DATA);
    ++sdk.registry_entries;
}
void h2_esp_platform_netif_unregister(esp_netif_t *netif) {
    assert(netif == sdk.netif);
    --sdk.registry_entries;
}
h2_pal_result_t h2_esp_platform_netif_reconcile_default(void) {
    ++sdk.reconciles;
    return H2_PAL_OK;
}

esp_modem_dce_t *esp_modem_new(const esp_modem_dte_config_t *dte,
    const esp_modem_dce_config_t *dce, esp_netif_t *netif) {
    assert(strcmp(dce->apn, "test.apn") == 0);
    sdk.dte = *dte;
    ++sdk.dces;
    esp_modem_dce_t *out = malloc(sizeof(*out));
    out->netif = netif;
    strcpy(out->apn, dce->apn);
    sdk.command_mode = true;
    return out;
}
void esp_modem_destroy(esp_modem_dce_t *dce) {
    assert(sdk.power_off != 0u);
    for (size_t i = 0u; i < 3u; ++i) assert(!sdk.handlers[i].active);
    --sdk.dces;
    free(dce);
}
esp_err_t esp_modem_at(esp_modem_dce_t *dce, const char *cmd, char *response, int timeout) {
    (void)dce;
    assert(timeout > 0);
    ++sdk.commands;
    if (sdk.long_response) {
        memset(response, 'X', 127u);
        response[127] = '\0';
        return ESP_OK;
    }
    const char *reply = "";
    if (strcmp(cmd, "AT+CPIN?") == 0) reply = "+CPIN: READY";
    else if (strcmp(cmd, "AT+CEREG?") == 0) reply = "+CEREG: 0,1";
    else if (strcmp(cmd, "AT+CGATT?") == 0) reply = "+CGATT: 1";
    else if (strcmp(cmd, "AT+CSQ") == 0) reply = "+CSQ: 10,0";
    else if (strcmp(cmd, "AT") != 0 && strcmp(cmd, "ATE0") != 0 &&
             strcmp(cmd, "AT+CMEE=2") != 0 && strncmp(cmd, "AT+CGDCONT=", 11u) != 0) {
        assert(!"unexpected native AT command");
    }
    strcpy(response, reply);
    return ESP_OK;
}
esp_err_t esp_modem_set_apn(esp_modem_dce_t *dce, const char *apn) {
    assert(sdk.command_mode);
    assert(strlen(apn) < sizeof(dce->apn));
    strcpy(dce->apn, apn);
    return ESP_OK;
}
esp_err_t esp_modem_set_mode(esp_modem_dce_t *dce, int mode) {
    if (mode == ESP_MODEM_MODE_DATA) {
        sdk.command_mode = false;
        strcpy(sdk.dial_apn, dce->apn); /* Generic setup uses its stored APN */
        if (!sdk.silent_data) got_ip(0x04030201u, true);
    } else {
        if (sdk.fail_command_mode) return ESP_ERR_TIMEOUT;
        if (!sdk.command_mode) {
            sdk.command_mode = true;
            if (!sdk.silent_stop) {
                esp_netif_t *netif = dce->netif;
                event(NETIF_PPP_STATUS, NETIF_PPP_PHASE_DEAD, &netif);
            }
        }
    }
    return ESP_OK;
}

static void *allocate(void *user, size_t size) {
    (void)user;
    void *out = malloc(size);
    if (out != NULL) ++sdk.allocations;
    return out;
}
static void release(void *user, void *ptr) {
    (void)user;
    --sdk.allocations;
    free(ptr);
}
static h2_pal_result_t mutex_create(void *user, const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
    (void)user;
    (void)config;
    *out_mutex = allocate(NULL, sizeof(**out_mutex));
    (*out_mutex)->locked = false;
    return H2_PAL_OK;
}
static h2_pal_result_t mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    assert(!mutex->locked);
    release(user, mutex);
    return H2_PAL_OK;
}
static h2_pal_result_t mutex_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(!mutex->locked);
    mutex->locked = true;
    return H2_PAL_OK;
}
static h2_pal_result_t mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(mutex->locked);
    mutex->locked = false;
    return H2_PAL_OK;
}
static h2_pal_result_t power(void *user, int enabled, uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    if (enabled) ++sdk.power_on;
    else {
        if (sdk.fail_power_off) return H2_PAL_ERR_IO;
        ++sdk.power_off;
    }
    return H2_PAL_OK;
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate, .free = release};
static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = mutex_create, .destroy_mutex = mutex_destroy,
    .lock_mutex = mutex_lock, .unlock_mutex = mutex_unlock,
};
static const h2_pal_sync_api_t sync = {.vtable = &sync_vtable};

static h2_esp_lierda_modem_t *create(void) {
    memset(&sdk, 0, sizeof(sdk));
    const h2_esp_lierda_modem_config_t config = {
        .model = H2_LIERDA_MODEM_MODEL_NT26KCNB20NNC,
        .uart_port = 1, .tx_gpio = 17, .rx_gpio = 18, .baud_rate = 115200u,
        .power = power, .allocator = &mem, .sync_api = &sync,
        .apn = {.apn = "test.apn"},
    };
    h2_esp_lierda_modem_t *modem = NULL;
    assert(h2_esp_lierda_modem_create(&config, &modem) == H2_PAL_OK);
    assert(sdk.power_on == 0u && sdk.groups == 0u);
    return modem;
}

static void destroy(h2_esp_lierda_modem_t *modem) {
    assert(h2_esp_lierda_modem_destroy(modem) == H2_PAL_OK);
    assert(sdk.allocations == 0u && sdk.groups == 0u && sdk.netifs == 0u && sdk.dces == 0u);
}

static void live_data_and_teardown(void) {
    h2_esp_lierda_modem_t *modem = create();
    h2_pal_modem_api_t *api = h2_esp_lierda_modem_api(modem);
    assert(h2_pal_modem_open(api, 0u) == H2_PAL_OK);
    assert(sdk.dte.uart_config.baud_rate == 115200);
    assert(sdk.dte.uart_config.flow_control == ESP_MODEM_FLOW_CONTROL_NONE);
    assert(sdk.dte.uart_config.rts_io_num == -1 && sdk.dte.uart_config.cts_io_num == -1);
    assert(h2_pal_modem_data_open(api, 1u) == H2_PAL_OK);
    h2_pal_modem_data_status_t data;
    assert(h2_pal_modem_get_data_status(api, &data) == H2_PAL_OK);
    assert(data.ip4_valid && data.dns1_ip4 == 1u && data.dns2_ip4 == 2u);
    const unsigned before = sdk.commands;
    h2_pal_modem_signal_t signal;
    assert(h2_pal_modem_get_signal(api, &signal) == H2_PAL_ERR_BUSY && sdk.commands == before);
    sdk.fail_power_off = true;
    assert(h2_esp_lierda_modem_destroy(modem) == H2_PAL_ERR_IO);
    assert(sdk.dces == 1u && sdk.groups == 1u && sdk.unregisters == 0u);
    sdk.fail_power_off = false;
    sdk.fail_unregister = true;
    assert(h2_esp_lierda_modem_destroy(modem) == H2_PAL_ERR_IO);
    assert(sdk.dces == 1u && sdk.netifs == 1u && sdk.allocations != 0u);
    sdk.fail_unregister = false;
    destroy(modem);
}

static void timeout_and_old_ip(void) {
    h2_esp_lierda_modem_t *modem = create();
    h2_pal_modem_api_t *api = h2_esp_lierda_modem_api(modem);
    assert(h2_pal_modem_open(api, 0u) == H2_PAL_OK);
    sdk.silent_data = true;
    assert(h2_pal_modem_data_open(api, 1u) == H2_PAL_ERR_TIMEOUT);
    got_ip(0x01020304u, false); /* queue replay without current SDK IP */
    h2_pal_modem_data_status_t data;
    assert(h2_pal_modem_get_data_status(api, &data) == H2_PAL_OK && !data.ip4_valid);
    assert(data.last_error == H2_PAL_ERR_TIMEOUT);
    got_ip(0x01020304u, true); /* failed dial cannot be revived before close */
    assert(h2_pal_modem_get_data_status(api, &data) == H2_PAL_OK);
    assert(data.state == H2_PAL_MODEM_DATA_CLOSING && !data.ip4_valid);
    assert(data.last_error == H2_PAL_ERR_TIMEOUT);
    sdk.fail_command_mode = true;
    assert(h2_pal_modem_data_close(api, 1u) == H2_PAL_ERR_TIMEOUT);
    h2_pal_modem_signal_t signal;
    assert(h2_pal_modem_get_signal(api, &signal) == H2_PAL_ERR_BUSY);
    got_ip(0x01020304u, true); /* closing cannot become OPEN again */
    assert(h2_pal_modem_get_data_status(api, &data) == H2_PAL_OK && !data.ip4_valid);
    sdk.fail_command_mode = false;
    assert(h2_pal_modem_data_close(api, 1u) == H2_PAL_OK);
    assert(sdk.netif->ip.ip.addr == 0u);
    assert(h2_pal_modem_get_signal(api, &signal) == H2_PAL_OK);
    sdk.silent_data = false;
    assert(h2_pal_modem_data_open(api, 1u) == H2_PAL_OK);
    const ip_event_got_ip_t lost = {.esp_netif = sdk.netif};
    const unsigned before_lost = sdk.reconciles;
    event(IP_EVENT, IP_EVENT_PPP_LOST_IP, (void *)&lost);
    assert(sdk.reconciles > before_lost);
    assert(h2_pal_modem_get_data_status(api, &data) == H2_PAL_OK && !data.ip4_valid);
    assert(h2_pal_modem_get_signal(api, &signal) == H2_PAL_ERR_BUSY);
    assert(h2_pal_modem_data_close(api, 1u) == H2_PAL_OK);
    sdk.long_response = true;
    assert(h2_pal_modem_get_signal(api, &signal) == H2_PAL_ERR_TRUNCATED);
    destroy(modem);
}

static void apn_and_late_stop_retry(void) {
    h2_esp_lierda_modem_t *modem = create();
    h2_pal_modem_api_t *api = h2_esp_lierda_modem_api(modem);
    assert(h2_pal_modem_open(api, 0u) == H2_PAL_OK);
    h2_pal_modem_apn_config_t apn = {.apn = "updated.apn"};
    assert(h2_pal_modem_set_apn(api, &apn) == H2_PAL_OK);
    assert(h2_pal_modem_data_open(api, 1u) == H2_PAL_OK);
    assert(strcmp(sdk.dial_apn, "updated.apn") == 0);
    sdk.silent_stop = true;
    assert(h2_pal_modem_data_close(api, 1u) == H2_PAL_ERR_TIMEOUT);
    assert(sdk.command_mode); /* repeated COMMAND will not send another DEAD */
    esp_netif_t *netif = sdk.netif;
    event(NETIF_PPP_STATUS, NETIF_PPP_PHASE_DEAD, &netif);
    assert(h2_pal_modem_data_close(api, 1u) == H2_PAL_OK);
    sdk.silent_stop = false;
    assert(h2_pal_modem_data_open(api, 1u) == H2_PAL_OK);
    sdk.fail_ip_clear = true;
    assert(h2_pal_modem_data_close(api, 1u) == H2_PAL_ERR_IO);
    sdk.fail_ip_clear = false;
    assert(h2_pal_modem_data_close(api, 1u) == H2_PAL_OK);
    destroy(modem);
}

static void authentication_backend(void) {
    h2_esp_lierda_modem_t *modem = create();
    h2_pal_modem_api_t *api = h2_esp_lierda_modem_api(modem);
    assert(h2_pal_modem_open(api, 0u) == H2_PAL_OK);
    const h2_pal_modem_apn_config_t apn = {
        .apn = "test.apn", .username = "test-user", .password = "fixture",
    };
    assert(h2_pal_modem_set_apn(api, &apn) == H2_PAL_OK);
    const h2_pal_result_t expected = CONFIG_LWIP_PPP_PAP_SUPPORT || CONFIG_LWIP_PPP_CHAP_SUPPORT
        ? H2_PAL_OK : H2_PAL_ERR_UNSUPPORTED;
    assert(h2_pal_modem_data_open(api, 1u) == expected);
    if (expected == H2_PAL_ERR_UNSUPPORTED) {
        h2_pal_modem_data_status_t data;
        assert(h2_pal_modem_get_data_status(api, &data) == H2_PAL_OK);
        assert(data.state == H2_PAL_MODEM_DATA_CLOSED && !data.ip4_valid);
        assert(sdk.command_mode); /* no unauthenticated DATA fallback */
    }
    destroy(modem);
}

int main(void) {
    live_data_and_teardown();
    timeout_and_old_ip();
    apn_and_late_stop_retry();
    authentication_backend();
    puts("Lierda native PPP state/teardown tests passed");
    return 0;
}
