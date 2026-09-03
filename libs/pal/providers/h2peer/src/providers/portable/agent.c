#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "base64.h"
#include "config.h"
#include "ice.h"
#include "ports.h"
#include "socket.h"
#include "stun.h"
#include "utils.h"

#define AGENT_POLL_TIMEOUT 10
#define AGENT_CONNCHECK_PERIOD 100
#define AGENT_CONNCHECK_TIMEOUT_MS 2000u
#define AGENT_STUN_RECV_MAXTIMES 1000

static void agent_turn_deallocate(Agent* agent);

static void agent_write_u32(uint8_t out[4], uint32_t value) {
  out[0] = (uint8_t)(value >> 24);
  out[1] = (uint8_t)(value >> 16);
  out[2] = (uint8_t)(value >> 8);
  out[3] = (uint8_t)value;
}

static void agent_write_u64(uint8_t out[8], uint64_t value) {
  for (size_t i = 0u; i < 8u; ++i) {
    out[i] = (uint8_t)(value >> (56u - 8u * i));
  }
}

static int agent_now_ms(Agent* agent, uint64_t* now_ms) {
  return h2_pal_time_get_monotonic_ms(agent->time, now_ms) == H2_PAL_OK
             ? 0
             : -1;
}

static int agent_stun_msg_create(
    Agent* agent, StunMessage* message, uint16_t type) {
  uint8_t transaction_id[12];
  if (h2_pal_crypto_random(
          agent->crypto, transaction_id, sizeof(transaction_id)) != H2_PAL_OK) {
    return -1;
  }
  stun_msg_create(message, type);
  stun_msg_set_transaction_id(message, transaction_id);
  return 0;
}

void agent_clear_candidates(Agent* agent) {
  ice_transport_close(&agent->transport);
  agent->selected_pair = NULL;
  agent->nominated_pair = NULL;
  agent->transport_pair = NULL;
  agent->local_candidates_count = 0;
  agent->remote_candidates_count = 0;
  agent->candidate_pairs_num = 0;
}

int agent_create(Agent* agent) {
  int ret;
  if (agent->net == NULL || agent->time == NULL || agent->crypto == NULL) {
    return -1;
  }
  agent->udp_sockets[0].fd = -1;
  agent->udp_sockets[1].fd = -1;
  memset(&agent->transport, 0, sizeof(agent->transport));
  agent->transport.tcp_socket.fd = -1;
  agent->transport_pair = NULL;
  if ((ret = udp_socket_open(&agent->udp_sockets[0], agent->net,
                             H2_PAL_NET_FAMILY_IPV4, 0u)) < 0) {
    H2_PEER_LOGE(agent->log, "Failed to create UDP socket.");
    return ret;
  }
  H2_PEER_LOGI(agent->log, "create IPv4 UDP socket: %d",
               agent->udp_sockets[0].fd);

#if CONFIG_IPV6
  if ((ret = udp_socket_open(&agent->udp_sockets[1], agent->net,
                             H2_PAL_NET_FAMILY_IPV6, 0u)) < 0) {
    H2_PEER_LOGE(agent->log, "Failed to create IPv6 UDP socket.");
    udp_socket_close(&agent->udp_sockets[0]);
    return ret;
  }
  H2_PEER_LOGI(agent->log, "create IPv6 UDP socket: %d",
               agent->udp_sockets[1].fd);
#endif

  agent_clear_candidates(agent);
  memset(agent->remote_ufrag, 0, sizeof(agent->remote_ufrag));
  memset(agent->remote_upwd, 0, sizeof(agent->remote_upwd));
  agent->turn_enabled = 0;
  agent->turn_permission_created = 0;
  h2_peer_turn_refresh_init(&agent->turn_refresh);
  return 0;
}

void agent_destroy(Agent* agent) {
  agent_turn_deallocate(agent);
  ice_transport_close(&agent->transport);
  agent->transport_pair = NULL;
  if (agent->udp_sockets[0].fd >= 0) {
    udp_socket_close(&agent->udp_sockets[0]);
  }

#if CONFIG_IPV6
  if (agent->udp_sockets[1].fd >= 0) {
    udp_socket_close(&agent->udp_sockets[1]);
  }
#endif
}

static int agent_socket_recv(
    Agent* agent, h2_pal_net_addr_t* addr, uint8_t* buf, int len) {
  memset(buf, 0, (size_t)len);
  int ret = udp_socket_recvfrom(&agent->udp_sockets[0], addr, buf, len,
                                AGENT_POLL_TIMEOUT);
#if CONFIG_IPV6
  if (ret == 0) {
    ret = udp_socket_recvfrom(&agent->udp_sockets[1], addr, buf, len, 0u);
  }
#endif
  return ret;
}

static int agent_socket_recv_attempts(
    Agent* agent, h2_pal_net_addr_t* addr, uint8_t* buf, int len,
    int maxtimes) {
  int ret = -1;
  int i = 0;
  for (i = 0; i < maxtimes; i++) {
    if ((ret = agent_socket_recv(agent, addr, buf, len)) != 0) {
      break;
    }
  }
  return ret;
}

static int agent_socket_send(
    Agent* agent, const h2_pal_net_addr_t* addr,
    const uint8_t* buf, int len) {
  switch (addr->family) {
    case H2_PAL_NET_FAMILY_IPV6:
      return udp_socket_sendto(&agent->udp_sockets[1], addr, buf, len);
    case H2_PAL_NET_FAMILY_IPV4:
      return udp_socket_sendto(&agent->udp_sockets[0], addr, buf, len);
    default:
      return -1;
  }
}

static int agent_addr_equal(const h2_pal_net_addr_t* left,
                            const h2_pal_net_addr_t* right) {
  if (left == NULL || right == NULL || left->family != right->family ||
      left->port != right->port) {
    return 0;
  }
  size_t ip_len = left->family == H2_PAL_NET_FAMILY_IPV4 ? 4u : 16u;
  return memcmp(left->ip, right->ip, ip_len) == 0;
}

static int agent_turn_write_peer_address(
    StunMessage* msg, const h2_pal_net_addr_t* peer_addr) {
  uint8_t value[20] = {0};
  uint8_t mask[16] = {0};
  if (stun_msg_get_xor_mask(msg, mask) != 0) {
    return -1;
  }
  int value_len = stun_set_mapped_address(
      value, sizeof(value), mask, peer_addr);
  if (value_len < 0) {
    return -1;
  }
  return stun_msg_write_attr(msg, STUN_ATTR_TYPE_XOR_PEER_ADDRESS,
                             (uint16_t)value_len, value);
}

static int agent_turn_write_auth(Agent* agent, StunMessage* msg) {
  return stun_msg_write_attr(msg, STUN_ATTR_TYPE_USERNAME,
                             (uint16_t)strlen(agent->turn_username),
                             agent->turn_username) == 0 &&
                 stun_msg_write_attr(msg, STUN_ATTR_TYPE_NONCE,
                                     (uint16_t)strlen(agent->turn_nonce),
                                     agent->turn_nonce) == 0 &&
                 stun_msg_write_attr(msg, STUN_ATTR_TYPE_REALM,
                                     (uint16_t)strlen(agent->turn_realm),
                                     agent->turn_realm) == 0
             ? 0
             : -1;
}

static int agent_turn_request(Agent* agent, StunMessage* send_msg,
                              StunMethod expected_method,
                              StunMessage* response) {
  StunMessage recv_msg;
  memset(&recv_msg, 0, sizeof(recv_msg));
  if (stun_msg_finish(agent->crypto, send_msg, STUN_CREDENTIAL_LONG_TERM,
                      agent->turn_credential,
                      strlen(agent->turn_credential)) != 0 ||
      agent_socket_send(agent, &agent->turn_server_addr, send_msg->buf,
                        (int)send_msg->size) < 0) {
    return -1;
  }
  int ret = agent_socket_recv_attempts(agent, NULL, recv_msg.buf,
                                       sizeof(recv_msg.buf),
                                       AGENT_STUN_RECV_MAXTIMES);
  if (ret <= 0) {
    return -1;
  }
  recv_msg.size = (size_t)ret;
  uint8_t send_transaction_id[STUN_TRANSACTION_ID_SIZE];
  uint8_t receive_transaction_id[STUN_TRANSACTION_ID_SIZE];
  if (stun_parse_msg_buf(&recv_msg) != 0 ||
      recv_msg.stunclass != STUN_CLASS_RESPONSE ||
      recv_msg.stunmethod != expected_method ||
      stun_msg_get_transaction_id(send_msg, send_transaction_id) != 0 ||
      stun_msg_get_transaction_id(&recv_msg, receive_transaction_id) != 0 ||
      memcmp(send_transaction_id, receive_transaction_id,
             sizeof(send_transaction_id)) != 0) {
    return -1;
  }
  if (response != NULL) {
    *response = recv_msg;
  }
  return 0;
}

static int agent_turn_create_permission(
    Agent* agent, const h2_pal_net_addr_t* peer_addr, int force_refresh) {
  if (!agent->turn_enabled) {
    return 0;
  }
  if (agent->turn_permission_created && !force_refresh) {
    return 0;
  }
  StunMessage msg;
  memset(&msg, 0, sizeof(msg));
  if (agent_stun_msg_create(
          agent, &msg, STUN_METHOD_CREATE_PERMISSION) != 0 ||
      agent_turn_write_peer_address(&msg, peer_addr) != 0 ||
      agent_turn_write_auth(agent, &msg) != 0 ||
      agent_turn_request(agent, &msg, STUN_METHOD_CREATE_PERMISSION, NULL) != 0) {
    return -1;
  }
  agent->turn_peer_addr = *peer_addr;
  agent->turn_permission_created = 1;
  uint64_t now_ms = 0u;
  if (agent_now_ms(agent, &now_ms) != 0) {
    return -1;
  }
  h2_peer_turn_record_permission(&agent->turn_refresh, now_ms);
  return 0;
}

static int agent_turn_maintain(Agent* agent) {
  if (!agent->turn_enabled) {
    return 0;
  }
  uint64_t now_ms = 0u;
  if (agent_now_ms(agent, &now_ms) != 0) {
    return -1;
  }
  if (h2_peer_turn_allocation_due(&agent->turn_refresh, now_ms)) {
    StunMessage request;
    StunMessage response;
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    if (agent_stun_msg_create(agent, &request, STUN_METHOD_REFRESH) != 0 ||
        agent_turn_write_auth(agent, &request) != 0 ||
        agent_turn_request(agent, &request, STUN_METHOD_REFRESH,
                           &response) != 0) {
      return -1;
    }
    h2_peer_turn_record_allocation(
        &agent->turn_refresh, now_ms, response.lifetime);
  }
  if (agent->turn_permission_created &&
      h2_peer_turn_permission_due(&agent->turn_refresh, now_ms) &&
      agent_turn_create_permission(agent, &agent->turn_peer_addr, 1) != 0) {
    return -1;
  }
  return 0;
}

static int agent_turn_send_data(
    Agent* agent, const h2_pal_net_addr_t* peer_addr,
                                const uint8_t* buf, int len) {
  StunMessage msg;
  if (len < 0 || (size_t)len > sizeof(msg.data) ||
      agent_turn_maintain(agent) != 0 ||
      agent_turn_create_permission(agent, peer_addr, 0) != 0) {
    return -1;
  }
  memset(&msg, 0, sizeof(msg));
  if (agent_stun_msg_create(
          agent, &msg, STUN_CLASS_INDICATION | STUN_METHOD_SEND) != 0 ||
      agent_turn_write_peer_address(&msg, peer_addr) != 0 ||
      stun_msg_write_attr(&msg, STUN_ATTR_TYPE_DATA, (uint16_t)len,
                          buf) != 0) {
    return -1;
  }
  int sent = agent_socket_send(agent, &agent->turn_server_addr, msg.buf,
                               (int)msg.size);
  return sent < 0 ? sent : len;
}

static int agent_send_to_peer(
    Agent* agent, const h2_pal_net_addr_t* peer_addr,
    const uint8_t* buf, int len) {
  if (agent->transport_pair == NULL || len < 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (agent->transport_pair->local->transport == ICE_TRANSPORT_TCP) {
    return ice_transport_send_packet(
        &agent->transport, buf, (size_t)len, 0u);
  }
  return agent->turn_enabled
             ? agent_turn_send_data(agent, peer_addr, buf, len)
             : agent_socket_send(agent, peer_addr, buf, len);
}

static void agent_turn_deallocate(Agent* agent) {
  if (agent == NULL || !agent->turn_enabled ||
      agent->turn_username[0] == '\0' || agent->turn_nonce[0] == '\0') {
    return;
  }
  StunMessage msg;
  const uint8_t lifetime[4] = {0};
  memset(&msg, 0, sizeof(msg));
  if (agent_stun_msg_create(agent, &msg, STUN_METHOD_REFRESH) == 0 &&
      stun_msg_write_attr(&msg, STUN_ATTR_TYPE_LIFETIME,
                          sizeof(lifetime), lifetime) == 0 &&
      agent_turn_write_auth(agent, &msg) == 0) {
    (void)agent_turn_request(agent, &msg, STUN_METHOD_REFRESH, NULL);
  }
  agent->turn_enabled = 0;
  agent->turn_permission_created = 0;
}

static int agent_create_host_addr(Agent* agent) {
  int i, j;
  const char* iface_prefx[] = {CONFIG_IFACE_PREFIX};
  IceCandidate* ice_candidate;
  h2_pal_net_family_t addr_type[] = {H2_PAL_NET_FAMILY_IPV4,
#if CONFIG_IPV6
                                     H2_PAL_NET_FAMILY_IPV6,
#endif
  };

  h2_pal_net_addr_t host_sources[2];
  size_t host_source_count = 0u;
  for (i = 0; i < sizeof(addr_type) / sizeof(addr_type[0]); i++) {
    for (j = 0; j < sizeof(iface_prefx) / sizeof(iface_prefx[0]); j++) {
      if (agent->local_candidates_count >= AGENT_MAX_CANDIDATES) {
        return -1;
      }
      ice_candidate = agent->local_candidates + agent->local_candidates_count;
      // only copy port and family to addr of ice candidate
      ice_candidate_create(ice_candidate, agent->local_candidates_count, ICE_CANDIDATE_TYPE_HOST,
                           &agent->udp_sockets[i].bind_addr);
      // if resolve host addr, add to local candidate
      if (ports_get_host_addr(agent->net, &ice_candidate->addr,
                              iface_prefx[j])) {
        if (host_source_count < sizeof(host_sources) / sizeof(host_sources[0])) {
          host_sources[host_source_count++] = ice_candidate->addr;
        }
        agent->local_candidates_count++;
      }
    }
  }

  for (size_t source = 0u; source < host_source_count; ++source) {
    if (agent->local_candidates_count >= AGENT_MAX_CANDIDATES) {
      return -1;
    }
    ice_candidate = agent->local_candidates + agent->local_candidates_count;
    ice_candidate_create_tcp_active(
        ice_candidate, agent->local_candidates_count, &host_sources[source]);
    agent->local_candidates_count++;
  }

  return 0;
}

static int agent_create_stun_addr(
    Agent* agent, const h2_pal_net_addr_t* serv_addr) {
  int ret = -1;
  h2_pal_net_addr_t bind_addr;
  StunMessage send_msg;
  StunMessage recv_msg;
  memset(&send_msg, 0, sizeof(send_msg));
  memset(&recv_msg, 0, sizeof(recv_msg));

  if (agent_stun_msg_create(
          agent, &send_msg, STUN_CLASS_REQUEST | STUN_METHOD_BINDING) != 0) {
    return -1;
  }

  ret = agent_socket_send(agent, serv_addr, send_msg.buf, send_msg.size);

  if (ret == -1) {
    H2_PEER_LOGE(agent->log, "Failed to send STUN Binding Request.");
    return ret;
  }

  ret = agent_socket_recv_attempts(agent, NULL, recv_msg.buf,
                                   sizeof(recv_msg.buf),
                                   AGENT_STUN_RECV_MAXTIMES);
  if (ret <= 0) {
    H2_PEER_LOGD(agent->log, "Failed to receive STUN Binding Response.");
    return ret;
  }

  recv_msg.size = (size_t)ret;
  if (stun_parse_msg_buf(&recv_msg) != 0 ||
      !h2_peer_net_addr_family_is_valid(recv_msg.mapped_addr.family)) {
    return -1;
  }
  bind_addr = recv_msg.mapped_addr;
  if (agent->local_candidates_count >= AGENT_MAX_CANDIDATES) {
    return -1;
  }
  IceCandidate* ice_candidate = agent->local_candidates + agent->local_candidates_count++;
  ice_candidate_create(ice_candidate, agent->local_candidates_count, ICE_CANDIDATE_TYPE_SRFLX, &bind_addr);
  return ret;
}

static int agent_create_turn_addr(
    Agent* agent, const h2_pal_net_addr_t* serv_addr,
    const char* username, const char* credential) {
  int ret = -1;
  const uint8_t requested_transport[4] = {17u, 0u, 0u, 0u};
  h2_pal_net_addr_t turn_addr;
  StunMessage send_msg;
  StunMessage recv_msg;
  memset(&recv_msg, 0, sizeof(recv_msg));
  memset(&send_msg, 0, sizeof(send_msg));
  if (agent_stun_msg_create(agent, &send_msg, STUN_METHOD_ALLOCATE) != 0 ||
      stun_msg_write_attr(
          &send_msg, STUN_ATTR_TYPE_REQUESTED_TRANSPORT,
          sizeof(requested_transport), requested_transport) != 0 ||
      stun_msg_write_attr(
          &send_msg, STUN_ATTR_TYPE_USERNAME,
          (uint16_t)strlen(username), username) != 0) {
    return -1;
  }

  ret = agent_socket_send(agent, serv_addr, send_msg.buf, send_msg.size);
  if (ret == -1) {
    H2_PEER_LOGE(agent->log, "Failed to send TURN Binding Request.");
    return -1;
  }

  ret = agent_socket_recv_attempts(agent, NULL, recv_msg.buf,
                                   sizeof(recv_msg.buf),
                                   AGENT_STUN_RECV_MAXTIMES);
  if (ret <= 0) {
    H2_PEER_LOGD(agent->log, "Failed to receive STUN Binding Response.");
    return ret;
  }

  recv_msg.size = (size_t)ret;
  if (stun_parse_msg_buf(&recv_msg) != 0) {
    return -1;
  }

  if (recv_msg.stunclass == STUN_CLASS_ERROR && recv_msg.stunmethod == STUN_METHOD_ALLOCATE) {
    memset(&send_msg, 0, sizeof(send_msg));
    if (agent_stun_msg_create(agent, &send_msg, STUN_METHOD_ALLOCATE) != 0 ||
        stun_msg_write_attr(
            &send_msg, STUN_ATTR_TYPE_REQUESTED_TRANSPORT,
            sizeof(requested_transport), requested_transport) != 0 ||
        stun_msg_write_attr(
            &send_msg, STUN_ATTR_TYPE_USERNAME,
            (uint16_t)strlen(username), username) != 0 ||
        stun_msg_write_attr(
            &send_msg, STUN_ATTR_TYPE_NONCE,
            (uint16_t)strlen(recv_msg.nonce), recv_msg.nonce) != 0 ||
        stun_msg_write_attr(
            &send_msg, STUN_ATTR_TYPE_REALM,
            (uint16_t)strlen(recv_msg.realm), recv_msg.realm) != 0) {
      return -1;
    }
    snprintf(agent->turn_realm, sizeof(agent->turn_realm), "%s", recv_msg.realm);
    snprintf(agent->turn_nonce, sizeof(agent->turn_nonce), "%s", recv_msg.nonce);
    if (stun_msg_finish(
            agent->crypto, &send_msg, STUN_CREDENTIAL_LONG_TERM,
            credential, strlen(credential)) != 0) {
      return -1;
    }
  } else {
    H2_PEER_LOGE(agent->log, "Invalid TURN Binding Response.");
    return -1;
  }

  ret = agent_socket_send(agent, serv_addr, send_msg.buf, send_msg.size);
  if (ret < 0) {
    H2_PEER_LOGE(agent->log, "Failed to send TURN Binding Request.");
    return -1;
  }

  ret = agent_socket_recv_attempts(agent, NULL, recv_msg.buf,
                                   sizeof(recv_msg.buf),
                                   AGENT_STUN_RECV_MAXTIMES);
  if (ret <= 0) {
    H2_PEER_LOGD(agent->log, "Failed to receive TURN Binding Response.");
    return ret;
  }

  recv_msg.size = (size_t)ret;
  if (stun_parse_msg_buf(&recv_msg) != 0) {
    return -1;
  }
  if (recv_msg.stunclass != STUN_CLASS_RESPONSE ||
      recv_msg.stunmethod != STUN_METHOD_ALLOCATE ||
      !h2_peer_net_addr_family_is_valid(recv_msg.relayed_addr.family)) {
    H2_PEER_LOGE(agent->log, "Invalid authenticated TURN Allocate Response.");
    return -1;
  }
  turn_addr = recv_msg.relayed_addr;
  agent->turn_server_addr = *serv_addr;
  snprintf(agent->turn_username, sizeof(agent->turn_username), "%s", username);
  snprintf(agent->turn_credential, sizeof(agent->turn_credential), "%s", credential);
  agent->turn_enabled = 1;
  agent->turn_permission_created = 0;
  uint64_t now_ms = 0u;
  if (agent_now_ms(agent, &now_ms) != 0) {
    agent->turn_enabled = 0;
    return -1;
  }
  h2_peer_turn_record_allocation(
      &agent->turn_refresh, now_ms, recv_msg.lifetime);
  if (agent->local_candidates_count >= AGENT_MAX_CANDIDATES) {
    agent_turn_deallocate(agent);
    return -1;
  }
  IceCandidate* ice_candidate = agent->local_candidates + agent->local_candidates_count++;
  ice_candidate_create(ice_candidate, agent->local_candidates_count, ICE_CANDIDATE_TYPE_RELAY, &turn_addr);
  return ret;
}

int agent_gather_candidate(
    Agent* agent,
    const char* urls,
    const char* username,
    const char* credential) {
  const char* pos;
  char hostname[64];
  char addr_string[H2_PEER_NET_ADDR_STRING_SIZE];
  int i;
  h2_pal_net_family_t addr_type[1] = {
      H2_PAL_NET_FAMILY_IPV4};  // ipv6 no need stun
  h2_pal_net_addr_t resolved_addr;
  memset(hostname, 0, sizeof(hostname));

  if (urls == NULL) {
    return agent_create_host_addr(agent);
  }

  if (strncmp(urls, "stun:", 5u) != 0 && strncmp(urls, "turn:", 5u) != 0) {
    H2_PEER_LOGE(agent->log, "Unsupported ICE server URL");
    return -1;
  }
  const char *hostname_start = urls + 5;
  if ((pos = strchr(hostname_start, ':')) == NULL) {
    H2_PEER_LOGE(agent->log, "Invalid URL");
    return -1;
  }

  size_t hostname_len = (size_t)(pos - hostname_start);
  if (hostname_len == 0u || hostname_len >= sizeof(hostname)) {
    H2_PEER_LOGE(agent->log, "ICE server hostname is too long");
    return -1;
  }

  char* port_end = NULL;
  long parsed_port = strtol(pos + 1, &port_end, 10);
  if (parsed_port <= 0 || parsed_port > UINT16_MAX || port_end == pos + 1 ||
      (*port_end != '\0' && *port_end != '?')) {
    H2_PEER_LOGE(agent->log, "Cannot parse port");
    return -1;
  }
  int port = (int)parsed_port;

  memcpy(hostname, hostname_start, hostname_len);
  hostname[hostname_len] = '\0';

  for (i = 0; i < sizeof(addr_type) / sizeof(addr_type[0]); i++) {
    if (ports_resolve_addr(agent->net, hostname, &resolved_addr) == 0) {
      if (resolved_addr.family != addr_type[i]) {
        continue;
      }
      resolved_addr.port = (uint16_t)port;
      h2_peer_net_addr_format(&resolved_addr, addr_string, sizeof(addr_string));
      H2_PEER_LOGI(agent->log, "Resolved stun/turn server %s:%d", addr_string,
                   port);

      if (strncmp(urls, "stun:", 5) == 0) {
        H2_PEER_LOGD(agent->log, "Create stun addr");
        return agent_create_stun_addr(agent, &resolved_addr) < 0 ? -1 : 0;
      } else if (strncmp(urls, "turn:", 5) == 0) {
        H2_PEER_LOGD(agent->log, "Create turn addr");
        return agent_create_turn_addr(agent, &resolved_addr, username,
                                      credential) < 0
                   ? -1
                   : 0;
      }
    }
  }
  return -1;
}

void agent_create_ice_credential(Agent* agent) {
  memset(agent->local_ufrag, 0, sizeof(agent->local_ufrag));
  memset(agent->local_upwd, 0, sizeof(agent->local_upwd));

  utils_random_string(agent->crypto, agent->local_ufrag, 4);
  utils_random_string(agent->crypto, agent->local_upwd, 24);
}

void agent_get_local_description(Agent* agent, char* description, int length) {
  for (int i = 0; i < agent->local_candidates_count; i++) {
    ice_candidate_to_description(&agent->local_candidates[i], description + strlen(description), length - strlen(description));
  }

  // remove last \n
  description[strlen(description)] = '\0';
  H2_PEER_LOGD(agent->log, "local ICE candidates: %d",
               agent->local_candidates_count);
}

int agent_send(Agent *agent, const uint8_t *buf, int len) {
  if (agent == NULL || agent->nominated_pair == NULL || len < 0 ||
      agent->transport_pair != agent->nominated_pair) {
    H2_PEER_LOGE(agent == NULL ? NULL : agent->log,
                 "agent send invalid state len %d nominated %d "
                 "transport_match %d",
                 len, agent != NULL && agent->nominated_pair != NULL,
                 agent != NULL &&
                     agent->transport_pair == agent->nominated_pair);
    return -1;
  }
  /* TURN/UDP is an existing transport policy: once configured, all UDP peer
   * traffic uses the allocation even when the active checklist entry points
   * at a host candidate.  TCP candidates remain direct; TURN/TCP is outside
   * this backend's contract. */
  int result =
      agent_send_to_peer(agent, &agent->nominated_pair->remote->addr, buf, len);
  if (result < 0 && result != H2_PAL_ERR_WOULD_BLOCK) {
    H2_PEER_LOGE(agent->log, "agent send failed %d len %d transport %d turn %d",
                 result, len, (int)agent->nominated_pair->local->transport,
                 agent->turn_enabled);
  }
  return result;
}

static int agent_create_binding_response(
    Agent* agent, StunMessage* msg, const h2_pal_net_addr_t* addr) {
  int size = 0;
  char username[584];
  uint8_t mapped_address[20];
  uint8_t mask[16] = {0};
  stun_msg_create(msg, STUN_CLASS_RESPONSE | STUN_METHOD_BINDING);
  stun_msg_set_transaction_id(msg, agent->transaction_id);
  snprintf(username, sizeof(username), "%s:%s", agent->local_ufrag, agent->remote_ufrag);
  if (stun_msg_get_xor_mask(msg, mask) != 0) {
    return -1;
  }
  size = stun_set_mapped_address(
      mapped_address, sizeof(mapped_address), mask, addr);
  if (size < 0) {
    return -1;
  }
  return stun_msg_write_attr(
             msg, STUN_ATTR_TYPE_XOR_MAPPED_ADDRESS,
             (uint16_t)size, mapped_address) == 0 &&
                 stun_msg_write_attr(
                     msg, STUN_ATTR_TYPE_USERNAME,
                     (uint16_t)strlen(username), username) == 0 &&
                 stun_msg_finish(
                     agent->crypto, msg, STUN_CREDENTIAL_SHORT_TERM,
                     agent->local_upwd, strlen(agent->local_upwd)) == 0
             ? 0
             : -1;
}

static int agent_create_binding_request(Agent* agent, StunMessage* msg) {
  uint64_t tie_breaker = 0;  // always be controlled
  // send binding request
  if (agent_stun_msg_create(
          agent, msg, STUN_CLASS_REQUEST | STUN_METHOD_BINDING) != 0) {
    return -1;
  }
  char username[584];
  memset(username, 0, sizeof(username));
  snprintf(username, sizeof(username), "%s:%s", agent->remote_ufrag, agent->local_ufrag);
  uint8_t priority[4];
  uint8_t encoded_tie_breaker[8];
  agent_write_u32(priority, (uint32_t)agent->nominated_pair->priority);
  agent_write_u64(encoded_tie_breaker, tie_breaker);
  if (stun_msg_write_attr(
          msg, STUN_ATTR_TYPE_USERNAME,
          (uint16_t)strlen(username), username) != 0 ||
      stun_msg_write_attr(
          msg, STUN_ATTR_TYPE_PRIORITY, sizeof(priority), priority) != 0) {
    return -1;
  }
  if (agent->mode == AGENT_MODE_CONTROLLING) {
    if (stun_msg_write_attr(
            msg, STUN_ATTR_TYPE_USE_CANDIDATE, 0, NULL) != 0 ||
        stun_msg_write_attr(
            msg, STUN_ATTR_TYPE_ICE_CONTROLLING,
            sizeof(encoded_tie_breaker), encoded_tie_breaker) != 0) {
      return -1;
    }
  } else {
    if (stun_msg_write_attr(
            msg, STUN_ATTR_TYPE_ICE_CONTROLLED,
            sizeof(encoded_tie_breaker), encoded_tie_breaker) != 0) {
      return -1;
    }
  }
  return stun_msg_finish(
      agent->crypto, msg, STUN_CREDENTIAL_SHORT_TERM,
      agent->remote_upwd, strlen(agent->remote_upwd));
}

void agent_process_stun_request(
    Agent* agent, StunMessage* stun_msg, const h2_pal_net_addr_t* addr) {
  StunMessage msg;
  switch (stun_msg->stunmethod) {
    case STUN_METHOD_BINDING:
      if (stun_msg_is_valid(
              agent->crypto, stun_msg->buf, stun_msg->size,
              agent->local_upwd) == 0) {
        if (stun_msg_get_transaction_id(
                stun_msg, agent->transaction_id) != 0) {
          return;
        }
        if (agent_create_binding_response(agent, &msg, addr) != 0 ||
            agent_send_to_peer(agent, addr, msg.buf, (int)msg.size) < 0) {
          return;
        }
        if (agent->nominated_pair != NULL &&
            agent->nominated_pair->state == ICE_CANDIDATE_STATE_SUCCEEDED) {
          (void)agent_now_ms(agent, &agent->ice_activity_time_ms);
        }
      }
      break;
    default:
      break;
  }
}

void agent_process_stun_response(Agent* agent, StunMessage* stun_msg) {
  switch (stun_msg->stunmethod) {
    case STUN_METHOD_BINDING:
      if (stun_msg_is_valid(
              agent->crypto, stun_msg->buf, stun_msg->size,
              agent->remote_upwd) == 0) {
        agent->nominated_pair->state = ICE_CANDIDATE_STATE_SUCCEEDED;
        (void)agent_now_ms(agent, &agent->ice_activity_time_ms);
      }
      break;
    default:
      break;
  }
}

int agent_async_receive_supported(const Agent* agent) {
  return agent != NULL && agent->transport_pair != NULL &&
         agent->transport.protocol == ICE_TRANSPORT_UDP &&
         !agent->turn_enabled;
}

int agent_recv_raw(Agent* agent, h2_pal_net_addr_t* addr, uint8_t* buf,
                   int len, uint32_t timeout_ms) {
  if (!agent_async_receive_supported(agent) || addr == NULL || buf == NULL ||
      len <= 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return ice_transport_receive_packet(
      &agent->transport, addr, buf, (size_t)len, timeout_ms);
}

int agent_process_received(Agent* agent, h2_pal_net_addr_t* addr, uint8_t* buf,
                           int len) {
  int ret = len;
  StunMessage stun_msg;
  if (agent == NULL || addr == NULL || buf == NULL || len <= 0) {
    return len;
  }
  if (ret > 0 && stun_probe(buf, (size_t)ret) == 0) {
    memcpy(stun_msg.buf, buf, ret);
    stun_msg.size = ret;
    if (stun_parse_msg_buf(&stun_msg) != 0) {
      return -1;
    }
    if (agent->turn_enabled &&
        stun_msg.stunclass == STUN_CLASS_INDICATION &&
        stun_msg.stunmethod == STUN_METHOD_DATA &&
        stun_msg.data_len <= (size_t)len) {
      if (!h2_peer_net_addr_family_is_valid(stun_msg.peer_addr.family)) {
        return -1;
      }
      memcpy(buf, stun_msg.data, stun_msg.data_len);
      *addr = stun_msg.peer_addr;
      ret = (int)stun_msg.data_len;
      if (stun_probe(buf, (size_t)ret) != 0) {
        return ret;
      }
      memset(&stun_msg, 0, sizeof(stun_msg));
      memcpy(stun_msg.buf, buf, (size_t)ret);
      stun_msg.size = (size_t)ret;
      if (stun_parse_msg_buf(&stun_msg) != 0) {
        return -1;
      }
    }
    switch (stun_msg.stunclass) {
      case STUN_CLASS_REQUEST:
        agent_process_stun_request(agent, &stun_msg, addr);
        break;
      case STUN_CLASS_RESPONSE:
        agent_process_stun_response(agent, &stun_msg);
        break;
      case STUN_CLASS_ERROR:
        break;
      default:
        break;
    }
    ret = 0;
  } else if (ret > 0 && agent->nominated_pair != NULL &&
             agent_addr_equal(addr, &agent->nominated_pair->remote->addr)) {
    (void)agent_now_ms(agent, &agent->ice_activity_time_ms);
  }
  return ret;
}

int agent_recv(Agent* agent, uint8_t* buf, int len, uint32_t timeout_ms) {
  int ret = -1;
  h2_pal_net_addr_t addr;
  if (agent_turn_maintain(agent) != 0) {
    return -1;
  }
  if (agent->transport_pair != NULL) {
    int flush_result = ice_transport_flush(&agent->transport, 0u);
    if (flush_result != H2_PAL_OK &&
        flush_result != H2_PAL_ERR_WOULD_BLOCK &&
        flush_result != H2_PAL_ERR_TIMEOUT) {
      return flush_result;
    }
    ret = ice_transport_receive_packet(
        &agent->transport, &addr, buf, (size_t)len, timeout_ms);
  } else {
    ret = agent_socket_recv(agent, &addr, buf, len);
  }
  return ret > 0 ? agent_process_received(agent, &addr, buf, ret) : ret;
}

int agent_set_remote_description(Agent* agent, char* description) {
  /*
  a=ice-ufrag:Iexb
  a=ice-pwd:IexbSoY7JulyMbjKwISsG9
  a=candidate:1 1 UDP 1 36.231.28.50 38143 typ srflx
  */
  int i;
  int previous_candidate_count = agent->remote_candidates_count;
  char previous_ufrag[sizeof(agent->remote_ufrag)];
  char previous_upwd[sizeof(agent->remote_upwd)];
  memcpy(previous_ufrag, agent->remote_ufrag, sizeof(previous_ufrag));
  memcpy(previous_upwd, agent->remote_upwd, sizeof(previous_upwd));

  char* line_start = description;
  char* line_end = NULL;

  while ((line_end = strstr(line_start, "\r\n")) != NULL) {
    if (strncmp(line_start, "a=ice-ufrag:", strlen("a=ice-ufrag:")) == 0) {
      size_t value_len =
          (size_t)(line_end - line_start) - strlen("a=ice-ufrag:");
      if (value_len >= sizeof(agent->remote_ufrag)) {
        goto rollback;
      }
      memcpy(agent->remote_ufrag,
             line_start + strlen("a=ice-ufrag:"), value_len);
      agent->remote_ufrag[value_len] = '\0';

    } else if (strncmp(line_start, "a=ice-pwd:", strlen("a=ice-pwd:")) == 0) {
      size_t value_len =
          (size_t)(line_end - line_start) - strlen("a=ice-pwd:");
      if (value_len >= sizeof(agent->remote_upwd)) {
        goto rollback;
      }
      memcpy(agent->remote_upwd,
             line_start + strlen("a=ice-pwd:"), value_len);
      agent->remote_upwd[value_len] = '\0';

    } else if (strncmp(line_start, "a=candidate:", strlen("a=candidate:")) == 0) {
      IceCandidate candidate;
      memset(&candidate, 0, sizeof(candidate));
      IceCandidateParseResult parse_result = ice_candidate_from_description(
          agent->net,
          &candidate,
          line_start, line_end);
      if (parse_result == ICE_CANDIDATE_PARSE_OK) {
        for (i = 0; i < agent->remote_candidates_count; i++) {
          if (ice_candidate_equal(
                  &agent->remote_candidates[i], &candidate)) {
            break;
          }
        }
        if (i == agent->remote_candidates_count) {
          if (agent->remote_candidates_count >= AGENT_MAX_CANDIDATES) {
            goto rollback;
          }
          agent->remote_candidates[agent->remote_candidates_count] = candidate;
          agent->remote_candidates_count++;
        }
      } else if (parse_result != ICE_CANDIDATE_PARSE_UNSUPPORTED) {
        goto rollback;
      }
    }

    line_start = line_end + 2;
  }

  if (agent_update_candidate_pairs(agent) != 0) {
    goto rollback;
  }
  H2_PEER_LOGD(agent->log, "remote ICE candidates: %d",
               agent->remote_candidates_count);
  return 0;

rollback:
  agent->remote_candidates_count = previous_candidate_count;
  memcpy(agent->remote_ufrag, previous_ufrag, sizeof(previous_ufrag));
  memcpy(agent->remote_upwd, previous_upwd, sizeof(previous_upwd));
  return -1;
}

int agent_update_candidate_pairs(Agent* agent) {
  int i, j;
  if (agent == NULL || agent->candidate_pairs_num < 0 ||
      agent->candidate_pairs_num > AGENT_MAX_CANDIDATE_PAIRS) {
    return -1;
  }

  int missing_pairs = 0;
  for (i = 0; i < agent->local_candidates_count; i++) {
    for (j = 0; j < agent->remote_candidates_count; j++) {
      if (!ice_candidate_pair_is_compatible(
              &agent->local_candidates[i], &agent->remote_candidates[j])) {
        continue;
      }
      int existing = 0;
      for (int pair_index = 0;
           pair_index < agent->candidate_pairs_num;
           ++pair_index) {
        if (agent->candidate_pairs[pair_index].local ==
                &agent->local_candidates[i] &&
            agent->candidate_pairs[pair_index].remote ==
                &agent->remote_candidates[j]) {
          existing = 1;
          break;
        }
      }
      if (!existing) {
        missing_pairs++;
      }
    }
  }
  if (missing_pairs > AGENT_MAX_CANDIDATE_PAIRS - agent->candidate_pairs_num) {
    H2_PEER_LOGE(agent->log, "ICE candidate pair capacity exceeded");
    return -1;
  }

  // Please set gather candidates before set remote description
  for (i = 0; i < agent->local_candidates_count; i++) {
    for (j = 0; j < agent->remote_candidates_count; j++) {
      if (!ice_candidate_pair_is_compatible(
              &agent->local_candidates[i], &agent->remote_candidates[j])) {
        continue;
      }
      int existing = 0;
      for (int pair_index = 0;
           pair_index < agent->candidate_pairs_num;
           ++pair_index) {
        if (agent->candidate_pairs[pair_index].local ==
                &agent->local_candidates[i] &&
            agent->candidate_pairs[pair_index].remote ==
                &agent->remote_candidates[j]) {
          existing = 1;
          break;
        }
      }
      if (existing) {
        continue;
      }
      IceCandidatePair pair;
      memset(&pair, 0, sizeof(pair));
      pair.local = &agent->local_candidates[i];
      pair.remote = &agent->remote_candidates[j];
      uint32_t minimum = pair.local->priority < pair.remote->priority
                             ? pair.local->priority
                             : pair.remote->priority;
      uint32_t maximum = pair.local->priority > pair.remote->priority
                             ? pair.local->priority
                             : pair.remote->priority;
      pair.priority = ((uint64_t)minimum << 32u) +
                      (uint64_t)maximum * 2u +
                      (pair.local->priority > pair.remote->priority ? 1u : 0u);
      pair.state = ICE_CANDIDATE_STATE_FROZEN;
      int insert = agent->candidate_pairs_num;
      while (insert > 0 &&
             agent->candidate_pairs[insert - 1].priority < pair.priority) {
        agent->candidate_pairs[insert] = agent->candidate_pairs[insert - 1];
        insert--;
      }
      if (agent->selected_pair != NULL &&
          agent->selected_pair >= &agent->candidate_pairs[insert] &&
          agent->selected_pair <
              &agent->candidate_pairs[agent->candidate_pairs_num]) {
        agent->selected_pair++;
      }
      if (agent->nominated_pair != NULL &&
          agent->nominated_pair >= &agent->candidate_pairs[insert] &&
          agent->nominated_pair <
              &agent->candidate_pairs[agent->candidate_pairs_num]) {
        agent->nominated_pair++;
      }
      if (agent->transport_pair != NULL &&
          agent->transport_pair >= &agent->candidate_pairs[insert] &&
          agent->transport_pair <
              &agent->candidate_pairs[agent->candidate_pairs_num]) {
        agent->transport_pair++;
      }
      agent->candidate_pairs[insert] = pair;
      agent->candidate_pairs_num++;
    }
  }
  H2_PEER_LOGD(agent->log, "candidate pairs num: %d",
               agent->candidate_pairs_num);
  return 0;
}

int agent_add_remote_candidate(
    Agent* agent, const IceCandidate* candidate) {
  if (agent == NULL || candidate == NULL) {
    return -1;
  }
  for (int i = 0; i < agent->remote_candidates_count; ++i) {
    if (ice_candidate_equal(&agent->remote_candidates[i], candidate)) {
      return 0;
    }
  }
  if (agent->remote_candidates_count >= AGENT_MAX_CANDIDATES) {
    return -1;
  }
  int added_index = agent->remote_candidates_count;
  agent->remote_candidates[added_index] = *candidate;
  agent->remote_candidates_count++;
  if (agent_update_candidate_pairs(agent) != 0) {
    agent->remote_candidates_count--;
    memset(&agent->remote_candidates[added_index], 0,
           sizeof(agent->remote_candidates[added_index]));
    return -1;
  }
  return 0;
}

int agent_connectivity_check(Agent* agent, uint32_t timeout_ms) {
  char addr_string[H2_PEER_NET_ADDR_STRING_SIZE];
  uint8_t buf[1400];
  StunMessage msg;

  if (agent->nominated_pair->state != ICE_CANDIDATE_STATE_INPROGRESS) {
    H2_PEER_LOGI(agent->log, "nominated pair is not in progress");
    return -1;
  }

  if (agent->nominated_pair->local->transport == ICE_TRANSPORT_TCP &&
      !agent->transport.tcp_connected) {
    int connect_result = ice_transport_progress_tcp_connect(
        &agent->transport, timeout_ms);
    if (connect_result == H2_PAL_ERR_TIMEOUT ||
        connect_result == H2_PAL_ERR_WOULD_BLOCK) {
      return -1;
    }
    if (connect_result != H2_PAL_OK) {
      agent->nominated_pair->state = ICE_CANDIDATE_STATE_FAILED;
      ice_transport_close(&agent->transport);
      agent->transport_pair = NULL;
      return -1;
    }
  }

  memset(&msg, 0, sizeof(msg));

  if (agent->nominated_pair->conncheck % AGENT_CONNCHECK_PERIOD == 0) {
    h2_peer_net_addr_format(&agent->nominated_pair->remote->addr, addr_string,
                            sizeof(addr_string));
    H2_PEER_LOGD(agent->log, "send binding request to remote ip: %s, port: %d",
                 addr_string, agent->nominated_pair->remote->addr.port);
    if (agent_create_binding_request(agent, &msg) != 0) {
      return -1;
    }
    int send_result = agent_send(agent, msg.buf, (int)msg.size);
    if (send_result < 0 && send_result != H2_PAL_ERR_WOULD_BLOCK &&
        send_result != H2_PAL_ERR_TIMEOUT) {
      agent->nominated_pair->state = ICE_CANDIDATE_STATE_FAILED;
      ice_transport_close(&agent->transport);
      agent->transport_pair = NULL;
      return -1;
    }
  }

  uint32_t check_wait_ms = timeout_ms < AGENT_POLL_TIMEOUT
                               ? timeout_ms
                               : AGENT_POLL_TIMEOUT;
  int receive_result = agent_recv(
      agent, buf, sizeof(buf), check_wait_ms);
  if (receive_result < 0) {
    agent->nominated_pair->state = ICE_CANDIDATE_STATE_FAILED;
    ice_transport_close(&agent->transport);
    agent->transport_pair = NULL;
    return -1;
  }

  if (agent->nominated_pair->state == ICE_CANDIDATE_STATE_SUCCEEDED) {
    agent->selected_pair = agent->nominated_pair;
    return 0;
  }

  return -1;
}

int agent_keepalive(Agent* agent) {
  if (agent == NULL || agent->nominated_pair == NULL ||
      agent->nominated_pair->state != ICE_CANDIDATE_STATE_SUCCEEDED) {
    return -1;
  }
  uint64_t now_ms = 0u;
  if (agent_now_ms(agent, &now_ms) != 0) {
    return -1;
  }
  if (now_ms >= agent->ice_keepalive_time_ms &&
      now_ms - agent->ice_keepalive_time_ms < CONFIG_KEEPALIVE_INTERVAL) {
    return 0;
  }
  StunMessage msg;
  memset(&msg, 0, sizeof(msg));
  agent->ice_keepalive_time_ms = now_ms;
  if (agent_create_binding_request(agent, &msg) != 0) {
    return -1;
  }
  int result = agent_send(agent, msg.buf, (int)msg.size);
  return result >= 0 || result == H2_PAL_ERR_WOULD_BLOCK ||
                 result == H2_PAL_ERR_TIMEOUT
             ? 0
             : -1;
}

static int agent_begin_candidate_pair(
    Agent* agent, IceCandidatePair* pair, uint32_t timeout_ms) {
  ice_transport_close(&agent->transport);
  agent->transport_pair = NULL;
  int result = 0;
  if (pair->local->transport == ICE_TRANSPORT_TCP) {
    h2_pal_net_addr_t source_addr = pair->local->addr;
    source_addr.port = 0u;
    result = ice_transport_init_tcp(
        &agent->transport, agent->net, &source_addr,
        &pair->remote->addr, timeout_ms);
  } else {
    UdpSocket* socket = pair->local->addr.family == H2_PAL_NET_FAMILY_IPV6
                            ? &agent->udp_sockets[1]
                            : &agent->udp_sockets[0];
    ice_transport_init_udp(
        &agent->transport, socket, &pair->remote->addr);
  }
  if (result != H2_PAL_OK && result != H2_PAL_ERR_TIMEOUT &&
      result != H2_PAL_ERR_WOULD_BLOCK) {
    ice_transport_close(&agent->transport);
    return -1;
  }
  if (agent_now_ms(agent, &agent->ice_activity_time_ms) != 0) {
    ice_transport_close(&agent->transport);
    return -1;
  }
  agent->ice_keepalive_time_ms = agent->ice_activity_time_ms;
  agent->transport_pair = pair;
  return 0;
}

int agent_select_candidate_pair(Agent* agent, uint32_t timeout_ms) {
  int i;
  for (i = 0; i < agent->candidate_pairs_num; i++) {
    if (agent->candidate_pairs[i].state == ICE_CANDIDATE_STATE_FROZEN) {
      // nominate this pair
      agent->nominated_pair = &agent->candidate_pairs[i];
      agent->candidate_pairs[i].conncheck = 0;
      agent->candidate_pairs[i].state = ICE_CANDIDATE_STATE_INPROGRESS;
      if (agent_begin_candidate_pair(
              agent, &agent->candidate_pairs[i], timeout_ms) != 0) {
        agent->candidate_pairs[i].state = ICE_CANDIDATE_STATE_FAILED;
        return 0;
      }
      return 0;
    } else if (agent->candidate_pairs[i].state == ICE_CANDIDATE_STATE_INPROGRESS) {
      agent->candidate_pairs[i].conncheck++;
      uint64_t now_ms = 0u;
      if (agent_now_ms(agent, &now_ms) == 0 &&
          now_ms >= agent->ice_activity_time_ms &&
          now_ms - agent->ice_activity_time_ms <
              AGENT_CONNCHECK_TIMEOUT_MS) {
        return 0;
      }
      agent->candidate_pairs[i].state = ICE_CANDIDATE_STATE_FAILED;
      if (agent->transport_pair == &agent->candidate_pairs[i]) {
        ice_transport_close(&agent->transport);
        agent->transport_pair = NULL;
      }
    } else if (agent->candidate_pairs[i].state == ICE_CANDIDATE_STATE_FAILED) {
    } else if (agent->candidate_pairs[i].state == ICE_CANDIDATE_STATE_SUCCEEDED) {
      agent->selected_pair = &agent->candidate_pairs[i];
      return 0;
    }
  }
  // all candidate pairs are failed
  return -1;
}
