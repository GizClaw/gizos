#include "h2_posix_pal_core.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
static int scenario;
static unsigned resolver_calls;
int h2_test_getaddrinfo(const char *host, const char *service,
    const struct addrinfo *hints, struct addrinfo **out) {
    (void)service; (void)hints;
    ++resolver_calls;
    assert(strcmp(host, "matrix") == 0 || strcmp(host, "notinvalid") == 0);
    *out = NULL;
    if (scenario == 2) return EAI_NONAME;
    size_t count = scenario == 1 ? 3u : 10u;
    struct addrinfo **tail = out;
    for (size_t i = 0u; i < count; ++i) {
        struct addrinfo *item = calloc(1u, sizeof(*item));
        assert(item);
        item->ai_family = scenario == 0 && i == 9u ? AF_INET : AF_INET6;
        item->ai_addrlen = item->ai_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
        item->ai_addr = calloc(1u, item->ai_addrlen);
        assert(item->ai_addr);
        if (item->ai_family == AF_INET) {
            struct sockaddr_in *addr = (void *)item->ai_addr;
            addr->sin_family = AF_INET;
            assert(inet_pton(AF_INET, "127.0.0.1", &addr->sin_addr) == 1);
        } else {
            struct sockaddr_in6 *addr = (void *)item->ai_addr;
            addr->sin6_family = AF_INET6;
            addr->sin6_addr.s6_addr[15] = scenario == 1 ? 1u : (uint8_t)(i + 1u);
            if (scenario == 1) {
                addr->sin6_addr.s6_addr[0] = 0xfeu;
                addr->sin6_addr.s6_addr[1] = 0x80u;
                addr->sin6_scope_id = i == 1u ? 8u : 7u;
            }
        }
        *tail = item;
        tail = &item->ai_next;
    }
    return 0;
}
void h2_test_freeaddrinfo(struct addrinfo *head) {
    while (head) {
        struct addrinfo *next = head->ai_next;
        free(head->ai_addr); free(head); head = next;
    }
}
int main(void) {
    const h2_pal_net_api_t *net = h2_posix_net_api();
    h2_pal_net_addr_list_t list;
    assert(h2_pal_net_resolve_all(net, "matrix", H2_PAL_NET_FAMILY_ANY, &list) == H2_PAL_OK);
    assert(list.count == H2_PAL_NET_ADDR_MAX && list.truncated);
    for (size_t i = 0u; i < list.count - 1u; ++i) {
        assert(list.addrs[i].family == H2_PAL_NET_FAMILY_IPV6);
        assert(list.addrs[i].ip[15] == i + 1u);
    }
    assert(list.addrs[list.count - 1u].family == H2_PAL_NET_FAMILY_IPV4);
    h2_pal_net_addr_t first;
    assert(h2_pal_net_resolve_addr(net, "matrix", &first) == H2_PAL_OK);
    assert(first.family == H2_PAL_NET_FAMILY_IPV4);
    assert(h2_pal_net_resolve_all(net, "matrix", H2_PAL_NET_FAMILY_IPV6, &list) == H2_PAL_OK);
    assert(list.count == H2_PAL_NET_ADDR_MAX && list.truncated);
    for (size_t i = 0u; i < list.count; ++i) assert(list.addrs[i].family == H2_PAL_NET_FAMILY_IPV6);
    memset(&list, 0xa5, sizeof(list));
    assert(h2_pal_net_resolve_all(net, "matrix", (h2_pal_net_family_t)99, &list) == H2_PAL_ERR_INVALID_ARG);
    assert(list.count == 0u && list.truncated == 0u);
    assert(h2_pal_net_resolve_all(net, "Case.LOCALHOST.", H2_PAL_NET_FAMILY_IPV6, &list) == H2_PAL_OK);
    assert(list.count == 1u && list.addrs[0].ip[15] == 1u);
    const unsigned before = resolver_calls;
    const char *invalid[] = {"invalid", "Case.INVALID.", "nonce.test.invalid"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memset(&list, 0xa5, sizeof(list));
        assert(h2_pal_net_resolve_all(net, invalid[i], H2_PAL_NET_FAMILY_ANY, &list) == H2_PAL_ERR_NOT_FOUND);
        assert(list.count == 0u && !list.truncated);
    }
    assert(resolver_calls == before);
    scenario = 1;
    assert(h2_pal_net_resolve_all(net, "matrix", H2_PAL_NET_FAMILY_ANY, &list) == H2_PAL_OK);
    assert(list.count == 2u && !list.truncated);
    assert(list.addrs[0].scope_id == 7u && list.addrs[1].scope_id == 8u);
    scenario = 2;
    memset(&list, 0xa5, sizeof(list));
    assert(h2_pal_net_resolve_all(net, "matrix", H2_PAL_NET_FAMILY_ANY, &list) == H2_PAL_ERR_NOT_FOUND);
    assert(list.count == 0u && list.truncated == 0u);
    assert(h2_pal_net_resolve_all(net, "notinvalid", H2_PAL_NET_FAMILY_ANY, &list) == H2_PAL_ERR_NOT_FOUND);
    assert(resolver_calls == before + 3u);
    return 0;
}
