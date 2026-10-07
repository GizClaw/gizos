#include "h2_bk_net_addr.h"
#include <limits.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <string.h>

int h2_bk_net_family(h2_pal_net_family_t family) {
  return family == H2_PAL_NET_FAMILY_IPV6   ? (LWIP_IPV6 ? AF_INET6 : -1)
         : family == H2_PAL_NET_FAMILY_IPV4 ? AF_INET
                                            : -1;
}

int h2_bk_net_to_sockaddr(const h2_pal_net_addr_t *addr,
                          struct sockaddr_storage *storage,
                          socklen_t *out_len) {
  if (addr == NULL || storage == NULL || out_len == NULL ||
      (addr->family != H2_PAL_NET_FAMILY_IPV4 &&
       addr->family != H2_PAL_NET_FAMILY_IPV6) ||
      (addr->family == H2_PAL_NET_FAMILY_IPV4 && addr->scope_id != 0u) ||
      (addr->family == H2_PAL_NET_FAMILY_IPV6 && addr->scope_id > UINT8_MAX) ||
      (addr->family == H2_PAL_NET_FAMILY_IPV6 && addr->ip[0] == 0xfeu &&
       (addr->ip[1] & 0xc0u) == 0x80u && addr->scope_id == 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(storage, 0, sizeof(*storage));
  if (addr->family == H2_PAL_NET_FAMILY_IPV6) {
#if LWIP_IPV6
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)storage;
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = htons(addr->port);
    sin6->sin6_scope_id = addr->scope_id;
    memcpy(&sin6->sin6_addr, addr->ip, 16u);
    *out_len = sizeof(*sin6);
#else
    return H2_PAL_ERR_UNSUPPORTED;
#endif
  } else {
    struct sockaddr_in *sin = (struct sockaddr_in *)storage;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(addr->port);
    memcpy(&sin->sin_addr, addr->ip, 4u);
    *out_len = sizeof(*sin);
  }
  return H2_PAL_OK;
}

int h2_bk_net_from_sockaddr(const struct sockaddr *sockaddr,
                            h2_pal_net_addr_t *out_addr) {
  if (sockaddr == NULL || out_addr == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_addr, 0, sizeof(*out_addr));
  if (sockaddr->sa_family == AF_INET6) {
#if LWIP_IPV6
    const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sockaddr;
    out_addr->family = H2_PAL_NET_FAMILY_IPV6;
    out_addr->port = ntohs(sin6->sin6_port);
    out_addr->scope_id = sin6->sin6_scope_id;
    memcpy(out_addr->ip, &sin6->sin6_addr, 16u);
    return H2_PAL_OK;
#else
    return H2_PAL_ERR_UNSUPPORTED;
#endif
  }
  if (sockaddr->sa_family == AF_INET) {
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sockaddr;
    out_addr->family = H2_PAL_NET_FAMILY_IPV4;
    out_addr->port = ntohs(sin->sin_port);
    memcpy(out_addr->ip, &sin->sin_addr, 4u);
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_UNSUPPORTED;
}

static int net_has_reserved_suffix(const char *host, const char *name) {
  size_t length = strlen(host);
  if (length != 0u && host[length - 1u] == '.')
    --length;
  const size_t local_length = strlen(name);
  if (length < local_length)
    return 0;
  size_t start = length - local_length;
  if (start != 0u && host[start - 1u] != '.')
    return 0;
  for (size_t i = 0u; i < local_length; ++i) {
    char c = host[start + i];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c + ('a' - 'A'));
    if (c != name[i])
      return 0;
  }
  return 1;
}

h2_pal_result_t h2_bk_net_resolve_all(void *user, const char *host,
                                      h2_pal_net_family_t family,
                                      h2_pal_net_addr_list_t *out_addrs) {
  (void)user;
  if (host == NULL || host[0] == '\0' || out_addrs == NULL ||
      (family != H2_PAL_NET_FAMILY_ANY && family != H2_PAL_NET_FAMILY_IPV4 &&
       family != H2_PAL_NET_FAMILY_IPV6)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_addrs, 0, sizeof(*out_addrs));
#if !LWIP_IPV6
  if (family == H2_PAL_NET_FAMILY_IPV6)
    return H2_PAL_ERR_UNSUPPORTED;
#endif
  if (net_has_reserved_suffix(host, "invalid"))
    return H2_PAL_ERR_NOT_FOUND;
  if (net_has_reserved_suffix(host, "localhost")) {
#if LWIP_IPV6
    if (family != H2_PAL_NET_FAMILY_IPV4) {
      h2_pal_net_addr_t *loopback = &out_addrs->addrs[out_addrs->count++];
      loopback->family = H2_PAL_NET_FAMILY_IPV6;
      loopback->ip[15] = 1u;
    }
#else
    if (family == H2_PAL_NET_FAMILY_IPV6)
      return H2_PAL_ERR_UNSUPPORTED;
#endif
    if (family != H2_PAL_NET_FAMILY_IPV6) {
      h2_pal_net_addr_t *loopback = &out_addrs->addrs[out_addrs->count++];
      loopback->family = H2_PAL_NET_FAMILY_IPV4;
      loopback->ip[0] = 127u;
      loopback->ip[3] = 1u;
    }
    return H2_PAL_OK;
  }
  char literal[64];
  const char *numeric = host;
  const char *zone = strchr(host, '%');
  uint32_t scope = 0u;
  if (zone != NULL) {
    size_t length = (size_t)(zone - host);
    if (length == 0u || length >= sizeof(literal) || zone[1] == '\0')
      return H2_PAL_ERR_INVALID_ARG;
    memcpy(literal, host, length);
    literal[length] = '\0';
    numeric = literal;
    for (const char *digit = zone + 1; *digit; ++digit) {
      if (*digit < '0' || *digit > '9' || scope > (UINT32_MAX - 9u) / 10u)
        return H2_PAL_ERR_INVALID_ARG;
      scope = scope * 10u + (uint32_t)(*digit - '0');
    }
    if (!scope)
      return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_net_addr_t address = {0};
  if (inet_pton(AF_INET6, numeric, address.ip) == 1) {
    if (family == H2_PAL_NET_FAMILY_IPV4)
      return H2_PAL_ERR_NOT_FOUND;
#if !LWIP_IPV6
    return H2_PAL_ERR_UNSUPPORTED;
#endif
    address.family = H2_PAL_NET_FAMILY_IPV6;
    address.scope_id = scope;
    out_addrs->addrs[0] = address;
    out_addrs->count = 1u;
    return H2_PAL_OK;
  }
  if (zone != NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (inet_pton(AF_INET, numeric, address.ip) == 1) {
    if (family == H2_PAL_NET_FAMILY_IPV6)
      return H2_PAL_ERR_NOT_FOUND;
    address.family = H2_PAL_NET_FAMILY_IPV4;
    out_addrs->addrs[0] = address;
    out_addrs->count = 1u;
    return H2_PAL_OK;
  }
  const h2_pal_net_family_t families[] = {H2_PAL_NET_FAMILY_IPV6,
                                          H2_PAL_NET_FAMILY_IPV4};
  h2_pal_result_t failure = H2_PAL_ERR_NOT_FOUND;
  for (size_t query = 0u; query < 2u; ++query) {
    if (family != H2_PAL_NET_FAMILY_ANY && family != families[query]) {
      continue;
    }
#if !LWIP_IPV6
    if (families[query] == H2_PAL_NET_FAMILY_IPV6) {
      if (family == H2_PAL_NET_FAMILY_IPV6) {
        return H2_PAL_ERR_UNSUPPORTED;
      }
      continue;
    }
#endif
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = h2_bk_net_family(families[query]);
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
      if (rc == EAI_MEMORY) {
        memset(out_addrs, 0, sizeof(*out_addrs));
        return H2_PAL_ERR_NO_MEMORY;
      }
      if (rc != EAI_NONAME) {
        failure = H2_PAL_ERR_IO;
      }
      continue;
    }
    for (const struct addrinfo *it = res; it != NULL; it = it->ai_next) {
      if (it->ai_family != AF_INET && it->ai_family != AF_INET6) {
        continue;
      }
      h2_pal_net_addr_t addr;
      if (h2_bk_net_from_sockaddr(it->ai_addr, &addr) != H2_PAL_OK) {
        continue;
      }
      if (family != H2_PAL_NET_FAMILY_ANY && addr.family != family)
        continue;
      size_t i = 0u;
      for (; i < out_addrs->count; ++i) {
        const h2_pal_net_addr_t *previous = &out_addrs->addrs[i];
        if (previous->family == addr.family &&
            previous->scope_id == addr.scope_id &&
            memcmp(previous->ip, addr.ip,
                   addr.family == H2_PAL_NET_FAMILY_IPV4 ? 4u : 16u) == 0) {
          break;
        }
      }
      if (i != out_addrs->count) {
        continue;
      }
      if (out_addrs->count == H2_PAL_NET_ADDR_MAX) {
        out_addrs->truncated = 1u;
        int family_present = 0;
        for (size_t kept = 0u; kept < out_addrs->count; ++kept)
          family_present |= out_addrs->addrs[kept].family == addr.family;
        /* Keep one answer of each family even when one DNS RRset fills
         * the bounded list. Relative resolver order of retained answers
         * is preserved, and truncation remains explicit. */
        if (!family_present && family == H2_PAL_NET_FAMILY_ANY)
          out_addrs->addrs[H2_PAL_NET_ADDR_MAX - 1u] = addr;
        continue;
      }
      out_addrs->addrs[out_addrs->count++] = addr;
    }
    freeaddrinfo(res);
  }
  return out_addrs->count != 0u ? H2_PAL_OK : failure;
}

/* Preserve the legacy IPv4 preference while accepting AAAA-only hosts. */
h2_pal_net_addr_t h2_bk_net_first_addr(const h2_pal_net_addr_list_t *addrs) {
  for (size_t i = 0u; i < addrs->count; ++i) {
    if (addrs->addrs[i].family == H2_PAL_NET_FAMILY_IPV4) {
      return addrs->addrs[i];
    }
  }
  return addrs->addrs[0];
}
