#ifndef ICE_H_
#define ICE_H_

#include <stdint.h>

#include "address.h"
#include "ice_transport.h"
#include "stun.h"
#include "h2/pal/net/h2_pal_net.h"

#define ICE_UFRAG_LENGTH 256
#define ICE_UPWD_LENGTH 256

typedef enum IceCandidateState {

  ICE_CANDIDATE_STATE_FROZEN,
  ICE_CANDIDATE_STATE_WAITING,
  ICE_CANDIDATE_STATE_INPROGRESS,
  ICE_CANDIDATE_STATE_SUCCEEDED,
  ICE_CANDIDATE_STATE_FAILED,

} IceCandidateState;

typedef enum IceCandidateType {

  ICE_CANDIDATE_TYPE_HOST,
  ICE_CANDIDATE_TYPE_SRFLX,
  ICE_CANDIDATE_TYPE_PRFLX,
  ICE_CANDIDATE_TYPE_RELAY,

} IceCandidateType;

typedef enum IceTcpType {
  ICE_TCP_TYPE_NONE = 0,
  ICE_TCP_TYPE_ACTIVE,
  ICE_TCP_TYPE_PASSIVE,
  ICE_TCP_TYPE_SO,
} IceTcpType;

typedef struct IceCandidate IceCandidate;

struct IceCandidate {
  char foundation[32 + 1];
  int component;
  uint32_t priority;
  IceTransportProtocol transport;
  IceTcpType tcp_type;
  IceCandidateType type;
  IceCandidateState state;
  h2_pal_net_addr_t addr;
  h2_pal_net_addr_t raddr;
};

typedef struct IceCandidatePair IceCandidatePair;

typedef enum IceCandidateParseResult {
  ICE_CANDIDATE_PARSE_INVALID = -1,
  ICE_CANDIDATE_PARSE_OK = 0,
  ICE_CANDIDATE_PARSE_UNSUPPORTED = 1,
} IceCandidateParseResult;

struct IceCandidatePair {
  IceCandidateState state;
  IceCandidate* local;
  IceCandidate* remote;
  int conncheck;
  uint64_t priority;
};

void ice_candidate_create(
    IceCandidate* ice_candidate, int foundation, IceCandidateType type,
    const h2_pal_net_addr_t* addr);

void ice_candidate_create_tcp_active(
    IceCandidate* ice_candidate, int foundation,
    const h2_pal_net_addr_t* source_addr);

int ice_candidate_equal(
    const IceCandidate* first, const IceCandidate* second);

int ice_candidate_pair_is_compatible(
    const IceCandidate* local, const IceCandidate* remote);

void ice_candidate_to_description(IceCandidate* candidate, char* description, int length);

IceCandidateParseResult ice_candidate_from_description(
    const h2_pal_net_api_t* net,
    IceCandidate* candidate,
    char* description,
    char* end);

#endif  // ICE_H_
