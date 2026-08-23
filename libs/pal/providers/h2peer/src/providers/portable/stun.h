#ifndef STUN_H_
#define STUN_H_

#include <stddef.h>
#include <stdint.h>

#include "address.h"
#include "h2/pal/os/h2_pal_crypto.h"

typedef struct StunMessage StunMessage;

#define STUN_ATTR_BUF_SIZE 2048
#define MAGIC_COOKIE 0x2112A442
#define STUN_FINGERPRINT_XOR 0x5354554e
#define STUN_HEADER_SIZE 20u
#define STUN_ATTRIBUTE_HEADER_SIZE 4u
#define STUN_TRANSACTION_ID_SIZE 12u

typedef enum StunClass {

  STUN_CLASS_REQUEST = 0x0000,
  STUN_CLASS_INDICATION = 0x0010,
  STUN_CLASS_RESPONSE = 0x0100,
  STUN_CLASS_ERROR = 0x0110,

} StunClass;

typedef enum StunMethod {

  STUN_METHOD_BINDING = 0x0001,
  STUN_METHOD_ALLOCATE = 0x0003,
  STUN_METHOD_REFRESH = 0x0004,
  STUN_METHOD_SEND = 0x0006,
  STUN_METHOD_DATA = 0x0007,
  STUN_METHOD_CREATE_PERMISSION = 0x0008,

} StunMethod;

typedef enum StunAttrType {

  STUN_ATTR_TYPE_MAPPED_ADDRESS = 0x0001,
  STUN_ATTR_TYPE_USERNAME = 0x0006,
  STUN_ATTR_TYPE_MESSAGE_INTEGRITY = 0x0008,
  STUN_ATTR_TYPE_ERROR_CODE = 0x0009,
  STUN_ATTR_TYPE_LIFETIME = 0x000d,
  STUN_ATTR_TYPE_XOR_PEER_ADDRESS = 0x0012,
  STUN_ATTR_TYPE_DATA = 0x0013,
  STUN_ATTR_TYPE_REALM = 0x0014,
  STUN_ATTR_TYPE_NONCE = 0x0015,
  STUN_ATTR_TYPE_XOR_RELAYED_ADDRESS = 0x0016,
  STUN_ATTR_TYPE_REQUESTED_TRANSPORT = 0x0019,
  STUN_ATTR_TYPE_XOR_MAPPED_ADDRESS = 0x0020,
  STUN_ATTR_TYPE_PRIORITY = 0x0024,
  STUN_ATTR_TYPE_USE_CANDIDATE = 0x0025,
  STUN_ATTR_TYPE_FINGERPRINT = 0x8028,
  STUN_ATTR_TYPE_ICE_CONTROLLED = 0x8029,
  STUN_ATTR_TYPE_ICE_CONTROLLING = 0x802a,
  STUN_ATTR_TYPE_SOFTWARE = 0x8022,
  // https://datatracker.ietf.org/doc/html/draft-thatcher-ice-network-cost-00
  STUN_ATTR_TYPE_NETWORK_COST = 0xc057,

} StunAttrType;

typedef enum StunCredential {

  STUN_CREDENTIAL_SHORT_TERM = 0x0001,
  STUN_CREDENTIAL_LONG_TERM = 0x0002,

} StunCredential;

typedef enum StunFamily {

  STUN_FAMILY_IPV4 = 0x01,
  STUN_FAMILY_IPV6 = 0x02,

} StunFamily;

struct StunMessage {
  StunClass stunclass;
  StunMethod stunmethod;
  uint32_t fingerprint;
  char message_integrity[20];
  char username[128];
  char realm[64];
  char nonce[64];
  h2_pal_net_addr_t mapped_addr;
  h2_pal_net_addr_t relayed_addr;
  h2_pal_net_addr_t peer_addr;
  uint8_t data[1500];
  size_t data_len;
  uint32_t lifetime;
  uint8_t buf[STUN_ATTR_BUF_SIZE];
  size_t size;
};

void stun_msg_create(StunMessage* msg, uint16_t type);

void stun_msg_set_transaction_id(
    StunMessage* msg, const uint8_t transaction_id[STUN_TRANSACTION_ID_SIZE]);

int stun_msg_get_transaction_id(
    const StunMessage* msg,
    uint8_t out_transaction_id[STUN_TRANSACTION_ID_SIZE]);

int stun_msg_get_xor_mask(const StunMessage* msg, uint8_t out_mask[16]);

int stun_set_mapped_address(
    uint8_t* value, size_t value_capacity, const uint8_t mask[16],
    const h2_pal_net_addr_t* addr);

int stun_get_mapped_address(
    const uint8_t* value, size_t value_len, const uint8_t mask[16],
    h2_pal_net_addr_t* out_addr);

int stun_parse_msg_buf(StunMessage* msg);

void stun_calculate_fingerprint(
    const uint8_t* buf, size_t len, uint32_t* fingerprint);

int stun_msg_write_attr(
    StunMessage* msg, StunAttrType type, uint16_t length,
    const void* value);

int stun_probe(const uint8_t* buf, size_t size);

int stun_msg_is_valid(const h2_pal_crypto_api_t* crypto,
                      const uint8_t* buf, size_t len, const char* password);

int stun_msg_finish(const h2_pal_crypto_api_t* crypto,
                    StunMessage* msg, StunCredential credential,
                    const char* password, size_t password_len);

#endif  // STUN_H_
