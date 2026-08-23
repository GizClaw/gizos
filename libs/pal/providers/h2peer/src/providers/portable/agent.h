#ifndef AGENT_H_
#define AGENT_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base64.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_time.h"
#include "ice.h"
#include "socket.h"
#include "stun.h"
#include "turn/h2_peer_turn.h"
#include "utils.h"

#ifndef AGENT_MAX_DESCRIPTION
#define AGENT_MAX_DESCRIPTION 40960
#endif

#ifndef AGENT_MAX_CANDIDATES
#define AGENT_MAX_CANDIDATES 10
#endif

#ifndef AGENT_MAX_CANDIDATE_PAIRS
#define AGENT_MAX_CANDIDATE_PAIRS 100
#endif

typedef enum AgentState {

  AGENT_STATE_GATHERING_ENDED = 0,
  AGENT_STATE_GATHERING_STARTED,
  AGENT_STATE_GATHERING_COMPLETED,

} AgentState;

typedef enum AgentMode {

  AGENT_MODE_CONTROLLED = 0,
  AGENT_MODE_CONTROLLING

} AgentMode;

typedef struct Agent Agent;

struct Agent {
  const h2_pal_log_api_t *log;
  const h2_pal_net_api_t *net;
  const h2_pal_time_api_t *time;
  const h2_pal_crypto_api_t *crypto;

  char remote_ufrag[ICE_UFRAG_LENGTH + 1];
  char remote_upwd[ICE_UPWD_LENGTH + 1];

  char local_ufrag[ICE_UFRAG_LENGTH + 1];
  char local_upwd[ICE_UPWD_LENGTH + 1];

  IceCandidate local_candidates[AGENT_MAX_CANDIDATES];
  IceCandidate remote_candidates[AGENT_MAX_CANDIDATES];

  int local_candidates_count;
  int remote_candidates_count;

  UdpSocket udp_sockets[2];

  h2_pal_net_addr_t host_addr;
  int b_host_addr;
  uint64_t ice_activity_time_ms;
  uint64_t ice_keepalive_time_ms;
  AgentState state;

  AgentMode mode;

  IceCandidatePair candidate_pairs[AGENT_MAX_CANDIDATE_PAIRS];
  IceCandidatePair* selected_pair;
  IceCandidatePair* nominated_pair;
  IceTransport transport;
  IceCandidatePair* transport_pair;

  int candidate_pairs_num;
  int use_candidate;
  uint8_t transaction_id[12];

  int turn_enabled;
  int turn_permission_created;
  h2_pal_net_addr_t turn_server_addr;
  h2_pal_net_addr_t turn_peer_addr;
  char turn_username[128];
  char turn_credential[128];
  char turn_realm[64];
  char turn_nonce[64];
  h2_peer_turn_refresh_t turn_refresh;
};

int agent_gather_candidate(
    Agent* agent,
    const char* urls,
    const char* username,
    const char* credential);

void agent_create_ice_credential(Agent* agent);

void agent_get_local_description(Agent* agent, char* description, int length);

int agent_send(Agent* agent, const uint8_t* buf, int len);

int agent_recv(Agent* agent, uint8_t* buf, int len, uint32_t timeout_ms);
int agent_async_receive_supported(const Agent* agent);
int agent_recv_raw(Agent* agent, h2_pal_net_addr_t* addr, uint8_t* buf, int len,
                   uint32_t timeout_ms);
int agent_process_received(Agent* agent, h2_pal_net_addr_t* addr, uint8_t* buf,
                           int len);

int agent_set_remote_description(Agent* agent, char* description);

int agent_add_remote_candidate(
    Agent* agent, const IceCandidate* candidate);

int agent_select_candidate_pair(Agent* agent, uint32_t timeout_ms);

int agent_connectivity_check(Agent* agent, uint32_t timeout_ms);

int agent_keepalive(Agent* agent);

void agent_clear_candidates(Agent* agent);

int agent_create(Agent* agent);

void agent_destroy(Agent* agent);

int agent_update_candidate_pairs(Agent* agent);

#endif  // AGENT_H_
