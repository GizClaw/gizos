#ifndef H2_PAL_NETIF_H
#define H2_PAL_NETIF_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/net/h2_pal_net.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_NETIF_NAME_MAX 16u
#define H2_PAL_NETIF_DNS_MAX 2u

typedef enum h2_pal_netif_kind {
    H2_PAL_NETIF_KIND_UNKNOWN = 0,
    H2_PAL_NETIF_KIND_LOOPBACK = 1,
    H2_PAL_NETIF_KIND_WIFI_STA = 2,
    H2_PAL_NETIF_KIND_WIFI_AP = 3,
    H2_PAL_NETIF_KIND_MODEM_DATA = 4,
    H2_PAL_NETIF_KIND_ETHERNET = 5,
} h2_pal_netif_kind_t;

typedef enum h2_pal_netif_flag {
    H2_PAL_NETIF_FLAG_UP = 1u << 0,
    H2_PAL_NETIF_FLAG_LINK_UP = 1u << 1,
    H2_PAL_NETIF_FLAG_HAS_IPV4 = 1u << 2,
    H2_PAL_NETIF_FLAG_HAS_IPV6 = 1u << 3,
    H2_PAL_NETIF_FLAG_DEFAULT_ROUTE = 1u << 4,
    H2_PAL_NETIF_FLAG_METERED = 1u << 5,
    H2_PAL_NETIF_FLAG_POINT_TO_POINT = 1u << 6,
} h2_pal_netif_flag_t;

typedef enum h2_pal_netif_ref_type {
    H2_PAL_NETIF_REF_DEFAULT = 0,
    H2_PAL_NETIF_REF_NAME = 1,
    H2_PAL_NETIF_REF_ID = 2,
    H2_PAL_NETIF_REF_KIND = 3,
} h2_pal_netif_ref_type_t;

typedef struct h2_pal_netif_ref {
    h2_pal_netif_ref_type_t type;
    h2_pal_netif_kind_t kind;
    uint32_t id;
    char name[H2_PAL_NETIF_NAME_MAX];
} h2_pal_netif_ref_t;

/**
 * @brief A committed change of the effective IPv4 default interface.
 *
 * The payload is borrowed for the duration of a system-event callback. A
 * valid side always contains a concrete NAME or ID reference. An invalid side
 * is completely zeroed and represents the absence of an IPv4 default route.
 */
typedef struct h2_pal_netif_default_changed {
    h2_pal_netif_ref_t previous;
    h2_pal_netif_ref_t current;
    uint8_t previous_valid;
    uint8_t current_valid;
} h2_pal_netif_default_changed_t;

typedef struct h2_pal_netif_dns_server {
    h2_pal_net_addr_t addr;
} h2_pal_netif_dns_server_t;

typedef struct h2_pal_netif_status {
    h2_pal_netif_ref_t ref;
    h2_pal_netif_kind_t kind;
    uint32_t flags;
    h2_pal_net_addr_t ipv4;
    h2_pal_net_addr_t netmask4;
    h2_pal_net_addr_t gateway4;
    h2_pal_net_addr_t ipv6;
    uint16_t mtu;
    uint8_t mac[6];
    uint8_t mac_valid;
    h2_pal_netif_dns_server_t dns[H2_PAL_NETIF_DNS_MAX];
    size_t dns_count;
} h2_pal_netif_status_t;

typedef struct h2_pal_netif_filter {
    h2_pal_netif_kind_t kind;
    const char *name;
    uint32_t id;
} h2_pal_netif_filter_t;

typedef int (*h2_pal_netif_list_fn)(
    void *user,
    const h2_pal_netif_ref_t *ref,
    const h2_pal_netif_status_t *status);

typedef struct h2_pal_netif_vtable {
    h2_pal_result_t (*list)(
        void *user,
        const h2_pal_netif_filter_t *filter,
        h2_pal_netif_list_fn on_netif,
        void *callback_user);
    h2_pal_result_t (*find)(
        void *user,
        const h2_pal_netif_filter_t *filter,
        h2_pal_netif_ref_t *out_ref);
    h2_pal_result_t (*get_status)(
        void *user,
        const h2_pal_netif_ref_t *ref,
        h2_pal_netif_status_t *out_status);
    h2_pal_result_t (*get_dns_servers)(
        void *user,
        const h2_pal_netif_ref_t *ref,
        h2_pal_netif_dns_server_t *out_servers,
        size_t max_servers,
        size_t *out_count);
} h2_pal_netif_vtable_t;

typedef struct h2_pal_netif_api {
    void *user;
    const h2_pal_netif_vtable_t *vtable;
} h2_pal_netif_api_t;

static inline h2_pal_netif_ref_t h2_pal_netif_default_ref(void) {
    h2_pal_netif_ref_t ref = {
        H2_PAL_NETIF_REF_DEFAULT,
        H2_PAL_NETIF_KIND_UNKNOWN,
        0u,
        {0},
    };
    return ref;
}

static inline int h2_pal_netif_ref_is_default(const h2_pal_netif_ref_t *ref) {
    return ref == NULL || ref->type == H2_PAL_NETIF_REF_DEFAULT;
}

static inline int h2_pal_netif_kind_is_valid(h2_pal_netif_kind_t kind) {
    return (unsigned int)kind <= (unsigned int)H2_PAL_NETIF_KIND_ETHERNET;
}

static inline int h2_pal_netif_ref_is_zero(const h2_pal_netif_ref_t *ref) {
    if (ref == NULL || ref->type != H2_PAL_NETIF_REF_DEFAULT ||
        ref->kind != H2_PAL_NETIF_KIND_UNKNOWN || ref->id != 0u) {
        return 0;
    }
    for (size_t i = 0u; i < sizeof(ref->name); ++i) {
        if (ref->name[i] != '\0') {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Return nonzero when ref is a canonical concrete event identity.
 */
static inline int h2_pal_netif_ref_is_concrete(const h2_pal_netif_ref_t *ref) {
    if (ref == NULL || !h2_pal_netif_kind_is_valid(ref->kind)) {
        return 0;
    }
    if (ref->type == H2_PAL_NETIF_REF_NAME) {
        return ref->id == 0u && ref->name[0] != '\0' &&
               memchr(ref->name, '\0', sizeof(ref->name)) != NULL;
    }
    if (ref->type == H2_PAL_NETIF_REF_ID) {
        if (ref->id == 0u) {
            return 0;
        }
        for (size_t i = 0u; i < sizeof(ref->name); ++i) {
            if (ref->name[i] != '\0') {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

/**
 * @brief Compare two canonical concrete references by authoritative identity.
 */
static inline int h2_pal_netif_ref_equal(
    const h2_pal_netif_ref_t *left,
    const h2_pal_netif_ref_t *right) {
    if (!h2_pal_netif_ref_is_concrete(left) ||
        !h2_pal_netif_ref_is_concrete(right) || left->type != right->type) {
        return 0;
    }
    if (left->type == H2_PAL_NETIF_REF_ID) {
        return left->id == right->id;
    }
    return strncmp(left->name, right->name, H2_PAL_NETIF_NAME_MAX) == 0;
}

/**
 * @brief Validate a default-interface transition payload.
 */
static inline int h2_pal_netif_default_changed_is_valid(
    const h2_pal_netif_default_changed_t *event) {
    if (event == NULL || event->previous_valid > 1u ||
        event->current_valid > 1u) {
        return 0;
    }
    if ((event->previous_valid != 0u &&
         !h2_pal_netif_ref_is_concrete(&event->previous)) ||
        (event->previous_valid == 0u &&
         !h2_pal_netif_ref_is_zero(&event->previous))) {
        return 0;
    }
    if ((event->current_valid != 0u &&
         !h2_pal_netif_ref_is_concrete(&event->current)) ||
        (event->current_valid == 0u &&
         !h2_pal_netif_ref_is_zero(&event->current))) {
        return 0;
    }
    return event->previous_valid == 0u || event->current_valid == 0u ||
           !h2_pal_netif_ref_equal(&event->previous, &event->current);
}

static inline h2_pal_result_t h2_pal_netif_list(
    const h2_pal_netif_api_t *api,
    const h2_pal_netif_filter_t *filter,
    h2_pal_netif_list_fn on_netif,
    void *callback_user) {
    if (on_netif == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->list == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->list(api->user, filter, on_netif, callback_user);
}

static inline h2_pal_result_t h2_pal_netif_find(
    const h2_pal_netif_api_t *api,
    const h2_pal_netif_filter_t *filter,
    h2_pal_netif_ref_t *out_ref) {
    if (filter == NULL || out_ref == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->find == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->find(api->user, filter, out_ref);
}

static inline h2_pal_result_t h2_pal_netif_get_status(
    const h2_pal_netif_api_t *api,
    const h2_pal_netif_ref_t *ref,
    h2_pal_netif_status_t *out_status) {
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_status == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_status(api->user, ref, out_status);
}

static inline h2_pal_result_t h2_pal_netif_get_dns_servers(
    const h2_pal_netif_api_t *api,
    const h2_pal_netif_ref_t *ref,
    h2_pal_netif_dns_server_t *out_servers,
    size_t max_servers,
    size_t *out_count) {
    if (out_count == NULL || (out_servers == NULL && max_servers != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_dns_servers == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_dns_servers(api->user, ref, out_servers, max_servers, out_count);
}

#ifdef __cplusplus
}
#endif

#endif
