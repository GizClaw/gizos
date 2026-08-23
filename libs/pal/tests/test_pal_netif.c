#include "h2/pal/net/h2_pal_netif.h"

#include <assert.h>
#include <string.h>

static h2_pal_netif_ref_t name_ref(
    const char *name,
    h2_pal_netif_kind_t kind) {
    h2_pal_netif_ref_t ref;
    memset(&ref, 0, sizeof(ref));
    ref.type = H2_PAL_NETIF_REF_NAME;
    ref.kind = kind;
    strncpy(ref.name, name, sizeof(ref.name) - 1u);
    return ref;
}

static h2_pal_netif_ref_t id_ref(
    uint32_t id,
    h2_pal_netif_kind_t kind) {
    h2_pal_netif_ref_t ref;
    memset(&ref, 0, sizeof(ref));
    ref.type = H2_PAL_NETIF_REF_ID;
    ref.kind = kind;
    ref.id = id;
    return ref;
}

static void test_concrete_refs(void) {
    h2_pal_netif_ref_t wifi = name_ref("en0", H2_PAL_NETIF_KIND_WIFI_STA);
    h2_pal_netif_ref_t wifi_unknown =
        name_ref("en0", H2_PAL_NETIF_KIND_UNKNOWN);
    h2_pal_netif_ref_t modem = id_ref(7u, H2_PAL_NETIF_KIND_MODEM_DATA);
    assert(h2_pal_netif_ref_is_concrete(&wifi));
    assert(h2_pal_netif_ref_is_concrete(&modem));
    assert(h2_pal_netif_ref_equal(&wifi, &wifi_unknown));
    assert(!h2_pal_netif_ref_equal(&wifi, &modem));

    h2_pal_netif_ref_t invalid = h2_pal_netif_default_ref();
    assert(!h2_pal_netif_ref_is_concrete(&invalid));
    invalid.type = H2_PAL_NETIF_REF_KIND;
    invalid.kind = H2_PAL_NETIF_KIND_WIFI_STA;
    assert(!h2_pal_netif_ref_is_concrete(&invalid));
    invalid = name_ref("en0", H2_PAL_NETIF_KIND_WIFI_STA);
    invalid.id = 1u;
    assert(!h2_pal_netif_ref_is_concrete(&invalid));
    memset(invalid.name, 'x', sizeof(invalid.name));
    invalid.id = 0u;
    assert(!h2_pal_netif_ref_is_concrete(&invalid));
    invalid = id_ref(0u, H2_PAL_NETIF_KIND_MODEM_DATA);
    assert(!h2_pal_netif_ref_is_concrete(&invalid));
    invalid = id_ref(1u, H2_PAL_NETIF_KIND_MODEM_DATA);
    invalid.name[0] = 'x';
    assert(!h2_pal_netif_ref_is_concrete(&invalid));
    invalid = name_ref("en0", (h2_pal_netif_kind_t)99);
    assert(!h2_pal_netif_ref_is_concrete(&invalid));
}

static void test_default_changed_payload(void) {
    h2_pal_netif_default_changed_t change;
    memset(&change, 0, sizeof(change));
    change.current = name_ref("en0", H2_PAL_NETIF_KIND_WIFI_STA);
    change.current_valid = 1u;
    assert(h2_pal_netif_default_changed_is_valid(&change));

    change.previous = change.current;
    change.previous_valid = 1u;
    assert(!h2_pal_netif_default_changed_is_valid(&change));

    change.current = id_ref(4u, H2_PAL_NETIF_KIND_MODEM_DATA);
    assert(h2_pal_netif_default_changed_is_valid(&change));

    change.current_valid = 0u;
    assert(!h2_pal_netif_default_changed_is_valid(&change));
    memset(&change.current, 0, sizeof(change.current));
    assert(h2_pal_netif_default_changed_is_valid(&change));

    change.current_valid = 2u;
    assert(!h2_pal_netif_default_changed_is_valid(&change));

    memset(&change, 0, sizeof(change));
    change.previous.id = 1u;
    assert(!h2_pal_netif_default_changed_is_valid(&change));
    memset(&change.previous, 0, sizeof(change.previous));
    change.current.name[0] = 'x';
    assert(!h2_pal_netif_default_changed_is_valid(&change));
}

int main(void) {
    test_concrete_refs();
    test_default_changed_payload();
    return 0;
}
