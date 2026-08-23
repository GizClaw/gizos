#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ice.h"
#include "ports.h"
#include "socket.h"
#include "utils.h"

#define ICE_CANDIDATE_DESCRIPTION_MAX 512

static uint8_t ice_candidate_type_preference(IceCandidateType type) {
  switch (type) {
    case ICE_CANDIDATE_TYPE_HOST:
      return 126;
    case ICE_CANDIDATE_TYPE_SRFLX:
      return 100;
    case ICE_CANDIDATE_TYPE_RELAY:
      return 0;
    default:
      return 0;
  }
}

static uint16_t ice_candidate_local_preference(IceCandidate* candidate) {
  if (candidate->transport == ICE_TRANSPORT_UDP) {
    return UINT16_MAX;
  }
  return candidate->tcp_type == ICE_TCP_TYPE_ACTIVE ? 32768u : 0u;
}

static void ice_candidate_priority(IceCandidate *candidate) {
  // priority = (2^24)*(type preference) + (2^8)*(local preference) + (256 -
  // component ID)
  candidate->priority =
      (1 << 24) * ice_candidate_type_preference(candidate->type) +
      (1 << 8) * ice_candidate_local_preference(candidate) +
      (256 - candidate->component);
}

void ice_candidate_create(
    IceCandidate* candidate, int foundation, IceCandidateType type,
    const h2_pal_net_addr_t* addr) {
  candidate->addr = *addr;
  candidate->type = type;

  snprintf(candidate->foundation, sizeof(candidate->foundation), "%d", foundation);
  // 1: RTP, 2: RTCP
  candidate->component = 1;
  candidate->transport = ICE_TRANSPORT_UDP;
  candidate->tcp_type = ICE_TCP_TYPE_NONE;
  ice_candidate_priority(candidate);
}

void ice_candidate_create_tcp_active(
    IceCandidate* candidate, int foundation,
    const h2_pal_net_addr_t* source_addr) {
  ice_candidate_create(
      candidate, foundation, ICE_CANDIDATE_TYPE_HOST, source_addr);
  candidate->transport = ICE_TRANSPORT_TCP;
  candidate->tcp_type = ICE_TCP_TYPE_ACTIVE;
  candidate->addr.port = 9u;
  ice_candidate_priority(candidate);
}

void ice_candidate_to_description(IceCandidate* candidate, char* description, int length) {
  char addr_string[H2_PEER_NET_ADDR_STRING_SIZE];
  char typ_raddr[128];

  memset(typ_raddr, 0, sizeof(typ_raddr));
  h2_peer_net_addr_format(
      &candidate->raddr, addr_string, sizeof(addr_string));

  switch (candidate->type) {
    case ICE_CANDIDATE_TYPE_HOST:
      snprintf(typ_raddr, sizeof(typ_raddr), "host");
      break;
    case ICE_CANDIDATE_TYPE_SRFLX:
      snprintf(typ_raddr, sizeof(typ_raddr), "srflx raddr %s rport %d", addr_string, candidate->raddr.port);
      break;
    case ICE_CANDIDATE_TYPE_RELAY:
      snprintf(typ_raddr, sizeof(typ_raddr), "relay raddr %s rport %d", addr_string, candidate->raddr.port);
    default:
      break;
  }

  h2_peer_net_addr_format(
      &candidate->addr, addr_string, sizeof(addr_string));
  snprintf(description, length, "a=candidate:%s %d %s %" PRIu32 " %s %d typ %s%s\r\n",
           candidate->foundation,
           candidate->component,
           candidate->transport == ICE_TRANSPORT_TCP ? "TCP" : "UDP",
           candidate->priority,
           addr_string,
           candidate->addr.port,
           typ_raddr,
           candidate->transport == ICE_TRANSPORT_TCP
               ? " tcptype active"
               : "");
}

static int ice_ascii_equal(const char* first, const char* second) {
  while (*first != '\0' && *second != '\0') {
    char a = *first >= 'A' && *first <= 'Z' ? (char)(*first + 'a' - 'A')
                                             : *first;
    char b = *second >= 'A' && *second <= 'Z' ? (char)(*second + 'a' - 'A')
                                               : *second;
    if (a != b) {
      return 0;
    }
    first++;
    second++;
  }
  return *first == '\0' && *second == '\0';
}

static char* ice_next_token(char** cursor) {
  while (**cursor == ' ' || **cursor == '\t') {
    (*cursor)++;
  }
  if (**cursor == '\0') {
    return NULL;
  }
  char* token = *cursor;
  while (**cursor != '\0' && **cursor != ' ' && **cursor != '\t') {
    (*cursor)++;
  }
  if (**cursor != '\0') {
    **cursor = '\0';
    (*cursor)++;
  }
  return token;
}

static int ice_parse_u32(const char* token, uint32_t* out_value) {
  char* end = NULL;
  unsigned long value = strtoul(token, &end, 10);
  if (token[0] == '\0' || end == token || *end != '\0' ||
      value > UINT32_MAX) {
    return -1;
  }
  *out_value = (uint32_t)value;
  return 0;
}

static int ice_parse_candidate_type(
    const char* token, IceCandidateType* out_type) {
  if (ice_ascii_equal(token, "host")) {
    *out_type = ICE_CANDIDATE_TYPE_HOST;
  } else if (ice_ascii_equal(token, "srflx")) {
    *out_type = ICE_CANDIDATE_TYPE_SRFLX;
  } else if (ice_ascii_equal(token, "prflx")) {
    *out_type = ICE_CANDIDATE_TYPE_PRFLX;
  } else if (ice_ascii_equal(token, "relay")) {
    *out_type = ICE_CANDIDATE_TYPE_RELAY;
  } else {
    return -1;
  }
  return 0;
}

IceCandidateParseResult ice_candidate_from_description(
    const h2_pal_net_api_t* net,
    IceCandidate* candidate,
    char* description,
    char* end) {
  if (net == NULL || candidate == NULL || description == NULL || end == NULL ||
      end < description) {
    return ICE_CANDIDATE_PARSE_INVALID;
  }
  size_t description_len = (size_t)(end - description);
  if (description_len == 0 ||
      description_len >= ICE_CANDIDATE_DESCRIPTION_MAX) {
    return ICE_CANDIDATE_PARSE_INVALID;
  }
  char bounded_description[ICE_CANDIDATE_DESCRIPTION_MAX];
  memcpy(bounded_description, description, description_len);
  bounded_description[description_len] = '\0';

  char* candidate_start = bounded_description;

  if (strncmp("a=", candidate_start, 2u) == 0) {
    candidate_start += 2u;
  }
  static const char prefix[] = "candidate:";
  if (strncmp(prefix, candidate_start, sizeof(prefix) - 1u) != 0) {
    return ICE_CANDIDATE_PARSE_INVALID;
  }
  candidate_start += sizeof(prefix) - 1u;

  char* cursor = candidate_start;
  char* tokens[32];
  size_t token_count = 0u;
  char* token = NULL;
  while ((token = ice_next_token(&cursor)) != NULL) {
    if (token_count >= sizeof(tokens) / sizeof(tokens[0])) {
      return ICE_CANDIDATE_PARSE_INVALID;
    }
    tokens[token_count++] = token;
  }
  if (token_count < 8u || ((token_count - 8u) & 1u) != 0u ||
      strlen(tokens[0]) > sizeof(candidate->foundation) - 1u ||
      !ice_ascii_equal(tokens[6], "typ")) {
    return ICE_CANDIDATE_PARSE_INVALID;
  }
  memset(candidate, 0, sizeof(*candidate));
  snprintf(candidate->foundation, sizeof(candidate->foundation), "%s", tokens[0]);
  uint32_t component = 0u;
  uint32_t port = 0u;
  if (ice_parse_u32(tokens[1], &component) != 0 || component == 0u ||
      component > INT32_MAX ||
      ice_parse_u32(tokens[3], &candidate->priority) != 0 ||
      ice_parse_u32(tokens[5], &port) != 0 || port == 0u ||
      port > UINT16_MAX ||
      ice_parse_candidate_type(tokens[7], &candidate->type) != 0) {
    return ICE_CANDIDATE_PARSE_INVALID;
  }
  candidate->component = (int)component;
  if (ice_ascii_equal(tokens[2], "udp")) {
    candidate->transport = ICE_TRANSPORT_UDP;
  } else if (ice_ascii_equal(tokens[2], "tcp")) {
    candidate->transport = ICE_TRANSPORT_TCP;
  } else {
    return ICE_CANDIDATE_PARSE_UNSUPPORTED;
  }
  size_t tcp_type_count = 0u;
  for (size_t i = 8u; i < token_count; i += 2u) {
    if (!ice_ascii_equal(tokens[i], "tcptype")) {
      continue;
    }
    tcp_type_count++;
    if (ice_ascii_equal(tokens[i + 1u], "active")) {
      candidate->tcp_type = ICE_TCP_TYPE_ACTIVE;
    } else if (ice_ascii_equal(tokens[i + 1u], "passive")) {
      candidate->tcp_type = ICE_TCP_TYPE_PASSIVE;
    } else if (ice_ascii_equal(tokens[i + 1u], "so")) {
      candidate->tcp_type = ICE_TCP_TYPE_SO;
    } else {
      return ICE_CANDIDATE_PARSE_INVALID;
    }
  }
  if ((candidate->transport == ICE_TRANSPORT_UDP && tcp_type_count != 0u) ||
      (candidate->transport == ICE_TRANSPORT_TCP && tcp_type_count != 1u)) {
    return ICE_CANDIDATE_PARSE_INVALID;
  }
  if (candidate->transport == ICE_TRANSPORT_TCP &&
      candidate->tcp_type != ICE_TCP_TYPE_PASSIVE) {
    return ICE_CANDIDATE_PARSE_UNSUPPORTED;
  }
  if (ports_resolve_addr(net, tokens[4], &candidate->addr) != 0) {
    return ICE_CANDIDATE_PARSE_INVALID;
  }
  if (!h2_peer_net_addr_family_is_valid(candidate->addr.family)) {
    return ICE_CANDIDATE_PARSE_INVALID;
  }
  candidate->addr.port = (uint16_t)port;
  return ICE_CANDIDATE_PARSE_OK;
}

int ice_candidate_equal(
    const IceCandidate* first, const IceCandidate* second) {
  if (first == NULL || second == NULL ||
      strcmp(first->foundation, second->foundation) != 0 ||
      first->component != second->component ||
      first->transport != second->transport ||
      first->type != second->type || first->tcp_type != second->tcp_type ||
      first->addr.family != second->addr.family ||
      first->addr.port != second->addr.port) {
    return 0;
  }
  size_t address_len = first->addr.family == H2_PAL_NET_FAMILY_IPV6 ? 16u : 4u;
  return memcmp(first->addr.ip, second->addr.ip, address_len) == 0;
}

int ice_candidate_pair_is_compatible(
    const IceCandidate* local, const IceCandidate* remote) {
  if (local == NULL || remote == NULL ||
      local->component != remote->component ||
      local->addr.family != remote->addr.family ||
      local->transport != remote->transport) {
    return 0;
  }
  return local->transport == ICE_TRANSPORT_UDP ||
         (local->tcp_type == ICE_TCP_TYPE_ACTIVE &&
          remote->tcp_type == ICE_TCP_TYPE_PASSIVE);
}
