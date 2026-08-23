#include "h2_esp_platform_core.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/def.h"
#include "lwip/ip_addr.h"

#include <string.h>

#define H2_ESP_NETIF_REGISTRY_MAX 8u
#define H2_ESP_NETIF_SNAPSHOT_MAX 16u

static const char *TAG = "h2_esp_netif";

typedef struct h2_esp_netif_registration {
    esp_netif_t *netif;
    h2_pal_netif_kind_t kind;
} h2_esp_netif_registration_t;

typedef struct h2_esp_netif_snapshot_entry {
    h2_pal_netif_status_t status;
    char key[H2_PAL_NETIF_NAME_MAX];
} h2_esp_netif_snapshot_entry_t;

typedef struct h2_esp_netif_snapshot {
    h2_esp_netif_snapshot_entry_t entries[H2_ESP_NETIF_SNAPSHOT_MAX];
    size_t count;
    int truncated;
} h2_esp_netif_snapshot_t;

static portMUX_TYPE s_netif_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_reconcile_mutex_storage;
static SemaphoreHandle_t s_reconcile_mutex;
static h2_esp_netif_registration_t
    s_netif_registry[H2_ESP_NETIF_REGISTRY_MAX];
static h2_pal_netif_ref_t s_default_ref;
static int s_default_known;
static int s_default_valid;

static h2_pal_netif_kind_t netif_kind(esp_netif_t *netif, const char *key) {
    h2_pal_netif_kind_t registered = H2_PAL_NETIF_KIND_UNKNOWN;
    portENTER_CRITICAL(&s_netif_lock);
    for (size_t i = 0u; i < H2_ESP_NETIF_REGISTRY_MAX; ++i) {
        if (s_netif_registry[i].netif == netif) {
            registered = s_netif_registry[i].kind;
            break;
        }
    }
    portEXIT_CRITICAL(&s_netif_lock);
    if (registered != H2_PAL_NETIF_KIND_UNKNOWN) return registered;
    if (key == NULL) return H2_PAL_NETIF_KIND_UNKNOWN;
    if (strstr(key, "STA") != NULL) return H2_PAL_NETIF_KIND_WIFI_STA;
    if (strstr(key, "AP") != NULL) return H2_PAL_NETIF_KIND_WIFI_AP;
    if (strstr(key, "PPP") != NULL) return H2_PAL_NETIF_KIND_MODEM_DATA;
    if (strstr(key, "ETH") != NULL) return H2_PAL_NETIF_KIND_ETHERNET;
    if (strstr(key, "LO") != NULL) return H2_PAL_NETIF_KIND_LOOPBACK;
    return H2_PAL_NETIF_KIND_UNKNOWN;
}

static h2_pal_result_t netif_ref(
    esp_netif_t *netif,
    h2_pal_netif_ref_t *out_ref) {
    memset(out_ref, 0, sizeof(*out_ref));
    int index = esp_netif_get_netif_impl_index(netif);
    const char *key = esp_netif_get_ifkey(netif);
    out_ref->kind = netif_kind(netif, key);
    if (index > 0) {
        out_ref->type = H2_PAL_NETIF_REF_ID;
        out_ref->id = (uint32_t)index;
        return H2_PAL_OK;
    }
    size_t len = key == NULL ? 0u : strnlen(key, H2_PAL_NETIF_NAME_MAX);
    if (len == 0u || len >= H2_PAL_NETIF_NAME_MAX) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    out_ref->type = H2_PAL_NETIF_REF_NAME;
    memcpy(out_ref->name, key, len + 1u);
    return H2_PAL_OK;
}

static void set_ipv4(h2_pal_net_addr_t *out, uint32_t network_order) {
    uint32_t host = lwip_ntohl(network_order);
    out->family = H2_PAL_NET_FAMILY_IPV4;
    out->ip[0] = (uint8_t)(host >> 24u);
    out->ip[1] = (uint8_t)(host >> 16u);
    out->ip[2] = (uint8_t)(host >> 8u);
    out->ip[3] = (uint8_t)host;
}

static h2_pal_result_t netif_status(
    esp_netif_t *netif,
    h2_pal_netif_status_t *out_status) {
    memset(out_status, 0, sizeof(*out_status));
    h2_pal_result_t rc = netif_ref(netif, &out_status->ref);
    if (rc != H2_PAL_OK) return rc;
    out_status->kind = out_status->ref.kind;
    if (out_status->kind == H2_PAL_NETIF_KIND_MODEM_DATA) {
        out_status->flags |= H2_PAL_NETIF_FLAG_POINT_TO_POINT;
    }
    if (esp_netif_is_netif_up(netif)) {
        out_status->flags |= H2_PAL_NETIF_FLAG_UP |
                             H2_PAL_NETIF_FLAG_LINK_UP;
    }
    if (netif == esp_netif_get_default_netif()) {
        out_status->flags |= H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
    }
    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0u) {
        set_ipv4(&out_status->ipv4, ip.ip.addr);
        set_ipv4(&out_status->netmask4, ip.netmask.addr);
        set_ipv4(&out_status->gateway4, ip.gw.addr);
        out_status->flags |= H2_PAL_NETIF_FLAG_HAS_IPV4;
    }
    if (esp_netif_get_mac(netif, out_status->mac) == ESP_OK) {
        out_status->mac_valid = 1u;
    }
    (void)esp_netif_get_mtu(netif, &out_status->mtu);
    const esp_netif_dns_type_t dns_types[] = {
        ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP,
    };
    for (size_t i = 0u; i < sizeof(dns_types) / sizeof(dns_types[0]) &&
         out_status->dns_count < H2_PAL_NETIF_DNS_MAX; ++i) {
        esp_netif_dns_info_t dns;
        memset(&dns, 0, sizeof(dns));
        if (esp_netif_get_dns_info(netif, dns_types[i], &dns) == ESP_OK &&
            IP_IS_V4(&dns.ip) && ip_2_ip4(&dns.ip)->addr != 0u) {
            set_ipv4(&out_status->dns[out_status->dns_count].addr,
                     ip_2_ip4(&dns.ip)->addr);
            ++out_status->dns_count;
        }
    }
    return H2_PAL_OK;
}

static esp_err_t capture_snapshot(void *ctx) {
    h2_esp_netif_snapshot_t *snapshot = ctx;
    esp_netif_t *netif = NULL;
    while ((netif = esp_netif_next_unsafe(netif)) != NULL) {
        if (snapshot->count >= H2_ESP_NETIF_SNAPSHOT_MAX) {
            snapshot->truncated = 1;
            break;
        }
        h2_esp_netif_snapshot_entry_t *entry =
            &snapshot->entries[snapshot->count];
        h2_pal_result_t rc = netif_status(netif, &entry->status);
        if (rc != H2_PAL_OK) continue;
        const char *key = esp_netif_get_ifkey(netif);
        size_t key_len = key == NULL ? 0u :
            strnlen(key, sizeof(entry->key));
        if (key_len > 0u && key_len < sizeof(entry->key)) {
            memcpy(entry->key, key, key_len + 1u);
        }
        ++snapshot->count;
    }
    return ESP_OK;
}

static h2_pal_result_t take_snapshot(h2_esp_netif_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    esp_err_t rc = esp_netif_tcpip_exec(capture_snapshot, snapshot);
    if (rc != ESP_OK) return H2_PAL_ERR_IO;
    return snapshot->truncated == 0 ? H2_PAL_OK : H2_PAL_ERR_FULL;
}

static int matches_filter(
    const h2_esp_netif_snapshot_entry_t *entry,
    const h2_pal_netif_filter_t *filter) {
    if (filter == NULL) return 1;
    if (filter->kind != H2_PAL_NETIF_KIND_UNKNOWN &&
        filter->kind != entry->status.kind) return 0;
    if (filter->id != 0u && filter->id != entry->status.ref.id) return 0;
    if (filter->name != NULL && filter->name[0] != '\0' &&
        strncmp(filter->name, entry->key,
                H2_PAL_NETIF_NAME_MAX) != 0) return 0;
    return 1;
}

static int matches_ref(
    const h2_esp_netif_snapshot_entry_t *entry,
    const h2_pal_netif_ref_t *ref) {
    if (h2_pal_netif_ref_is_default(ref)) {
        return (entry->status.flags & H2_PAL_NETIF_FLAG_DEFAULT_ROUTE) != 0u;
    }
    if (ref->type == H2_PAL_NETIF_REF_ID) {
        return ref->id == entry->status.ref.id;
    }
    if (ref->type == H2_PAL_NETIF_REF_NAME) {
        return strncmp(ref->name, entry->key,
                       H2_PAL_NETIF_NAME_MAX) == 0;
    }
    return ref->type == H2_PAL_NETIF_REF_KIND &&
           ref->kind == entry->status.kind;
}

static h2_pal_result_t esp_netif_list(
    void *user,
    const h2_pal_netif_filter_t *filter,
    h2_pal_netif_list_fn on_netif,
    void *callback_user) {
    (void)user;
    h2_esp_netif_snapshot_t snapshot;
    h2_pal_result_t rc = take_snapshot(&snapshot);
    if (rc != H2_PAL_OK) return rc;
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if (!matches_filter(&snapshot.entries[i], filter)) continue;
        if (on_netif(callback_user, &snapshot.entries[i].status.ref,
                     &snapshot.entries[i].status) != 0) {
            return H2_PAL_EXIT;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t esp_netif_find(
    void *user,
    const h2_pal_netif_filter_t *filter,
    h2_pal_netif_ref_t *out_ref) {
    (void)user;
    h2_esp_netif_snapshot_t snapshot;
    h2_pal_result_t rc = take_snapshot(&snapshot);
    if (rc != H2_PAL_OK) return rc;
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if (matches_filter(&snapshot.entries[i], filter)) {
            *out_ref = snapshot.entries[i].status.ref;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t esp_netif_get_status(
    void *user,
    const h2_pal_netif_ref_t *ref,
    h2_pal_netif_status_t *out_status) {
    (void)user;
    h2_esp_netif_snapshot_t snapshot;
    h2_pal_result_t rc = take_snapshot(&snapshot);
    if (rc != H2_PAL_OK) return rc;
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if (matches_ref(&snapshot.entries[i], ref)) {
            *out_status = snapshot.entries[i].status;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t esp_netif_get_dns_servers(
    void *user,
    const h2_pal_netif_ref_t *ref,
    h2_pal_netif_dns_server_t *out_servers,
    size_t max_servers,
    size_t *out_count) {
    (void)user;
    *out_count = 0u;
    h2_esp_netif_snapshot_t snapshot;
    h2_pal_result_t rc = take_snapshot(&snapshot);
    if (rc != H2_PAL_OK) return rc;
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if (matches_ref(&snapshot.entries[i], ref)) {
            size_t count = snapshot.entries[i].status.dns_count < max_servers
                ? snapshot.entries[i].status.dns_count : max_servers;
            if (count > 0u) {
                memcpy(out_servers, snapshot.entries[i].status.dns,
                       count * sizeof(out_servers[0]));
            }
            *out_count = count;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

typedef struct h2_esp_default_read {
    h2_pal_netif_ref_t ref;
    h2_pal_result_t result;
    int valid;
} h2_esp_default_read_t;

static esp_err_t read_default_in_tcpip(void *ctx) {
    h2_esp_default_read_t *read = ctx;
    esp_netif_t *netif = esp_netif_get_default_netif();
    memset(&read->ref, 0, sizeof(read->ref));
    read->valid = 0;
    read->result = H2_PAL_OK;
    if (netif == NULL) return ESP_OK;
    read->result = netif_ref(netif, &read->ref);
    if (read->result == H2_PAL_OK) read->valid = 1;
    return ESP_OK;
}

static h2_pal_result_t read_default(
    h2_pal_netif_ref_t *out_ref,
    int *out_valid) {
    h2_esp_default_read_t read;
    memset(&read, 0, sizeof(read));
    if (esp_netif_tcpip_exec(read_default_in_tcpip, &read) != ESP_OK) {
        return H2_PAL_ERR_IO;
    }
    *out_ref = read.ref;
    *out_valid = read.valid;
    return read.result;
}

h2_pal_result_t h2_esp_platform_netif_monitor_init(void) {
    if (s_reconcile_mutex == NULL) {
        s_reconcile_mutex =
            xSemaphoreCreateMutexStatic(&s_reconcile_mutex_storage);
        if (s_reconcile_mutex == NULL) return H2_PAL_ERR_NO_MEMORY;
    }
    if (xSemaphoreTake(s_reconcile_mutex, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_TIMEOUT;
    }
    h2_pal_netif_ref_t ref;
    int valid;
    h2_pal_result_t rc = read_default(&ref, &valid);
    if (rc == H2_PAL_OK) {
        portENTER_CRITICAL(&s_netif_lock);
        s_default_ref = ref;
        s_default_valid = valid;
        s_default_known = 1;
        portEXIT_CRITICAL(&s_netif_lock);
    }
    xSemaphoreGive(s_reconcile_mutex);
    return rc;
}

void h2_esp_platform_netif_register(
    void *netif_handle,
    h2_pal_netif_kind_t kind) {
    esp_netif_t *netif = netif_handle;
    if (netif == NULL || !h2_pal_netif_kind_is_valid(kind)) return;
    portENTER_CRITICAL(&s_netif_lock);
    for (size_t i = 0u; i < H2_ESP_NETIF_REGISTRY_MAX; ++i) {
        if (s_netif_registry[i].netif == netif ||
            s_netif_registry[i].netif == NULL) {
            s_netif_registry[i].netif = netif;
            s_netif_registry[i].kind = kind;
            break;
        }
    }
    portEXIT_CRITICAL(&s_netif_lock);
}

void h2_esp_platform_netif_unregister(void *netif_handle) {
    portENTER_CRITICAL(&s_netif_lock);
    for (size_t i = 0u; i < H2_ESP_NETIF_REGISTRY_MAX; ++i) {
        if (s_netif_registry[i].netif == netif_handle) {
            memset(&s_netif_registry[i], 0, sizeof(s_netif_registry[i]));
            break;
        }
    }
    portEXIT_CRITICAL(&s_netif_lock);
}

void h2_esp_platform_netif_monitor_deinit(void) {
    const BaseType_t locked = s_reconcile_mutex == NULL ? pdFALSE :
        xSemaphoreTake(s_reconcile_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_netif_lock);
    s_default_known = 0;
    s_default_valid = 0;
    memset(&s_default_ref, 0, sizeof(s_default_ref));
    portEXIT_CRITICAL(&s_netif_lock);
    if (locked == pdTRUE) {
        xSemaphoreGive(s_reconcile_mutex);
    }
}

h2_pal_result_t h2_esp_platform_netif_reconcile_default(void) {
    if (s_reconcile_mutex == NULL) return H2_PAL_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_reconcile_mutex, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_TIMEOUT;
    }
    h2_pal_netif_ref_t next;
    int next_valid;
    h2_pal_result_t rc = read_default(&next, &next_valid);
    if (rc != H2_PAL_OK) {
        ESP_LOGW(TAG, "default route query failed rc=%d", rc);
        xSemaphoreGive(s_reconcile_mutex);
        return rc;
    }
    h2_pal_netif_default_changed_t change;
    memset(&change, 0, sizeof(change));
    int changed = 0;
    portENTER_CRITICAL(&s_netif_lock);
    if (s_default_known != 0 &&
        (next_valid != s_default_valid ||
         (next_valid != 0 &&
          !h2_pal_netif_ref_equal(&next, &s_default_ref)))) {
        changed = 1;
        change.previous_valid = (uint8_t)s_default_valid;
        change.current_valid = (uint8_t)next_valid;
        if (s_default_valid != 0) change.previous = s_default_ref;
        if (next_valid != 0) change.current = next;
        s_default_ref = next;
        s_default_valid = next_valid;
    }
    portEXIT_CRITICAL(&s_netif_lock);
    if (changed != 0) {
        h2_pal_system_event_t event = {
            .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
            .payload = &change,
            .payload_size = sizeof(change),
        };
        rc = h2_pal_system_event_post(
            h2_esp_platform_system_event_api(), &event, 0u);
        if (rc != H2_PAL_OK) {
            ESP_LOGW(TAG, "default route event failed rc=%d", rc);
        }
    }
    xSemaphoreGive(s_reconcile_mutex);
    return rc;
}

const h2_pal_netif_api_t *h2_esp_platform_netif_api(void) {
    static const h2_pal_netif_vtable_t vtable = {
        .list = esp_netif_list,
        .find = esp_netif_find,
        .get_status = esp_netif_get_status,
        .get_dns_servers = esp_netif_get_dns_servers,
    };
    static const h2_pal_netif_api_t api = {.user = NULL, .vtable = &vtable};
    return &api;
}
