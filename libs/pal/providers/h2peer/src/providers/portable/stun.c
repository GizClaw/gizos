#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stun.h"
#include "utils.h"

uint32_t CRC32_TABLE[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535,
    0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd,
    0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d,
    0x6ddde4eb, 0xf4d4b551, 0x83d385c7, 0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,
    0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4,
    0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59, 0x26d930ac,
    0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab,
    0xb6662d3d, 0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f,
    0x9fbfe4a5, 0xe8b8d433, 0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb,
    0x086d3d2d, 0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea,
    0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65, 0x4db26158, 0x3ab551ce,
    0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a,
    0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409,
    0xce61e49f, 0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739,
    0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
    0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1, 0xf00f9344, 0x8708a3d2, 0x1e01f268,
    0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0,
    0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8,
    0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef,
    0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703,
    0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7,
    0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d, 0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a,
    0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae,
    0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777, 0x88085ae6,
    0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d,
    0x3e6e77db, 0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5,
    0x47b2cf7f, 0x30b5ffe9, 0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605,
    0xcdd70693, 0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d};

static uint16_t stun_read_u16(const uint8_t* value) {
  return (uint16_t)(((uint16_t)value[0] << 8) | value[1]);
}

static uint32_t stun_read_u32(const uint8_t* value) {
  return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
         ((uint32_t)value[2] << 8) | value[3];
}

static void stun_write_u16(uint8_t* out, uint16_t value) {
  out[0] = (uint8_t)(value >> 8);
  out[1] = (uint8_t)value;
}

static void stun_write_u32(uint8_t* out, uint32_t value) {
  out[0] = (uint8_t)(value >> 24);
  out[1] = (uint8_t)(value >> 16);
  out[2] = (uint8_t)(value >> 8);
  out[3] = (uint8_t)value;
}

void stun_msg_create(StunMessage* msg, uint16_t type) {
  if (msg == NULL) {
    return;
  }
  memset(msg->buf, 0, STUN_HEADER_SIZE);
  stun_write_u16(msg->buf, type);
  stun_write_u32(msg->buf + 4u, MAGIC_COOKIE);
  stun_write_u32(msg->buf + 8u, CRC32_TABLE[1]);
  stun_write_u32(msg->buf + 12u, CRC32_TABLE[2]);
  stun_write_u32(msg->buf + 16u, CRC32_TABLE[3]);
  msg->size = STUN_HEADER_SIZE;
}

void stun_msg_set_transaction_id(
    StunMessage* msg,
    const uint8_t transaction_id[STUN_TRANSACTION_ID_SIZE]) {
  if (msg != NULL && transaction_id != NULL &&
      msg->size >= STUN_HEADER_SIZE) {
    memcpy(msg->buf + 8u, transaction_id, STUN_TRANSACTION_ID_SIZE);
  }
}

int stun_msg_get_transaction_id(
    const StunMessage* msg,
    uint8_t out_transaction_id[STUN_TRANSACTION_ID_SIZE]) {
  if (msg == NULL || out_transaction_id == NULL ||
      msg->size < STUN_HEADER_SIZE) {
    return -1;
  }
  memcpy(out_transaction_id, msg->buf + 8u, STUN_TRANSACTION_ID_SIZE);
  return 0;
}

int stun_msg_get_xor_mask(const StunMessage* msg, uint8_t out_mask[16]) {
  if (msg == NULL || out_mask == NULL ||
      stun_probe(msg->buf, msg->size) != 0) {
    return -1;
  }
  stun_write_u32(out_mask, MAGIC_COOKIE);
  memcpy(out_mask + 4u, msg->buf + 8u, STUN_TRANSACTION_ID_SIZE);
  return 0;
}

int stun_set_mapped_address(
    uint8_t* value, size_t value_capacity, const uint8_t mask[16],
    const h2_pal_net_addr_t* addr) {
  if (value == NULL || mask == NULL || addr == NULL) {
    return -1;
  }
  size_t address_len;
  size_t value_len;

  switch (addr->family) {
    case H2_PAL_NET_FAMILY_IPV6:
      value_len = 20u;
      address_len = 16u;
      break;
    case H2_PAL_NET_FAMILY_IPV4:
      value_len = 8u;
      address_len = 4u;
      break;
    default:
      return -1;
  }
  if (value_capacity < value_len) {
    return -1;
  }
  memset(value, 0, value_len);
  value[1] = addr->family == H2_PAL_NET_FAMILY_IPV6
                 ? STUN_FAMILY_IPV6
                 : STUN_FAMILY_IPV4;
  for (size_t i = 0u; i < address_len; ++i) {
    value[4u + i] = addr->ip[i] ^ mask[i];
  }

  uint16_t encoded_port = addr->port ^ stun_read_u16(mask);
  stun_write_u16(value + 2u, encoded_port);
  return (int)value_len;
}

int stun_get_mapped_address(
    const uint8_t* value, size_t value_len, const uint8_t mask[16],
    h2_pal_net_addr_t* out_addr) {
  if (value == NULL || mask == NULL || out_addr == NULL) {
    return -1;
  }
  memset(out_addr, 0, sizeof(*out_addr));
  if (value_len < 4u || value[0] != 0u) {
    return -1;
  }
  uint8_t family = value[1];
  size_t address_len;

  switch (family) {
    case STUN_FAMILY_IPV6:
      if (value_len != 20u) {
        return -1;
      }
      out_addr->family = H2_PAL_NET_FAMILY_IPV6;
      address_len = 16u;
      break;
    case STUN_FAMILY_IPV4:
      if (value_len != 8u) {
        return -1;
      }
      out_addr->family = H2_PAL_NET_FAMILY_IPV4;
      address_len = 4u;
      break;
    default:
      return -1;
  }
  for (size_t i = 0u; i < address_len; ++i) {
    out_addr->ip[i] = value[4u + i] ^ mask[i];
  }

  out_addr->port = stun_read_u16(value + 2u) ^ stun_read_u16(mask);
  return 0;
}

static void stun_copy_text_attr(
    char* dst, size_t dst_len, const char* src, size_t src_len) {
  size_t copy_len = src_len < dst_len - 1u ? src_len : dst_len - 1u;
  memset(dst, 0, dst_len);
  memcpy(dst, src, copy_len);
}

int stun_parse_msg_buf(StunMessage* msg) {
  if (msg == NULL || msg->size < STUN_HEADER_SIZE ||
      msg->size > sizeof(msg->buf)) {
    return -1;
  }
  if (stun_read_u32(msg->buf + 4u) != MAGIC_COOKIE) {
    return -1;
  }
  size_t attributes_len = stun_read_u16(msg->buf + 2u);
  if ((attributes_len & 3u) != 0u ||
      attributes_len > msg->size - STUN_HEADER_SIZE) {
    return -1;
  }
  size_t length = attributes_len + STUN_HEADER_SIZE;
  size_t pos = STUN_HEADER_SIZE;

  memset(&msg->mapped_addr, 0, sizeof(msg->mapped_addr));
  memset(&msg->relayed_addr, 0, sizeof(msg->relayed_addr));
  memset(&msg->peer_addr, 0, sizeof(msg->peer_addr));
  memset(msg->message_integrity, 0, sizeof(msg->message_integrity));
  memset(msg->username, 0, sizeof(msg->username));
  memset(msg->realm, 0, sizeof(msg->realm));
  memset(msg->nonce, 0, sizeof(msg->nonce));
  msg->fingerprint = 0u;
  msg->lifetime = 0u;
  msg->data_len = 0u;

  msg->stunclass = (StunClass)stun_read_u16(msg->buf);
  if ((msg->stunclass & STUN_CLASS_ERROR) == STUN_CLASS_ERROR) {
    msg->stunclass = STUN_CLASS_ERROR;
  } else if ((msg->stunclass & STUN_CLASS_INDICATION) == STUN_CLASS_INDICATION) {
    msg->stunclass = STUN_CLASS_INDICATION;
  } else if ((msg->stunclass & STUN_CLASS_RESPONSE) == STUN_CLASS_RESPONSE) {
    msg->stunclass = STUN_CLASS_RESPONSE;
  } else if ((msg->stunclass & STUN_CLASS_REQUEST) == STUN_CLASS_REQUEST) {
    msg->stunclass = STUN_CLASS_REQUEST;
  }

  uint16_t encoded_type = stun_read_u16(msg->buf);
  msg->stunmethod = (StunMethod)((encoded_type & 0x000f) |
                                 ((encoded_type & 0x00e0) >> 1) |
                                 ((encoded_type & 0x3e00) >> 2));

  while (pos < length) {
    if (length - pos < STUN_ATTRIBUTE_HEADER_SIZE) {
      return -1;
    }
    uint16_t attr_type = stun_read_u16(msg->buf + pos);
    size_t attr_len = stun_read_u16(msg->buf + pos + 2u);
    size_t padded_len = (attr_len + 3u) & ~(size_t)3u;
    if (padded_len < attr_len ||
        padded_len > length - pos - STUN_ATTRIBUTE_HEADER_SIZE) {
      return -1;
    }
    const uint8_t* value = msg->buf + pos + STUN_ATTRIBUTE_HEADER_SIZE;
    uint8_t mask[16] = {0};

    switch (attr_type) {
      case STUN_ATTR_TYPE_MAPPED_ADDRESS:
        if (stun_get_mapped_address(
                value, attr_len, mask, &msg->mapped_addr) != 0) {
          return -1;
        }
        break;
      case STUN_ATTR_TYPE_USERNAME:
        stun_copy_text_attr(msg->username, sizeof(msg->username),
                            (const char*)value, attr_len);
        break;
      case STUN_ATTR_TYPE_MESSAGE_INTEGRITY:
        if (attr_len != sizeof(msg->message_integrity)) {
          return -1;
        }
        memcpy(msg->message_integrity, value,
               sizeof(msg->message_integrity));
        break;
      case STUN_ATTR_TYPE_LIFETIME:
        if (attr_len != sizeof(uint32_t)) {
          return -1;
        }
        msg->lifetime = stun_read_u32(value);
        break;
      case STUN_ATTR_TYPE_ERROR_CODE:
        break;
      case STUN_ATTR_TYPE_REALM:
        stun_copy_text_attr(msg->realm, sizeof(msg->realm), (const char *)value,
                            attr_len);
        break;
      case STUN_ATTR_TYPE_NONCE:
        stun_copy_text_attr(msg->nonce, sizeof(msg->nonce), (const char *)value,
                            attr_len);
        break;
      case STUN_ATTR_TYPE_XOR_RELAYED_ADDRESS:
        if (stun_msg_get_xor_mask(msg, mask) != 0 ||
            stun_get_mapped_address(value, attr_len, mask,
                                    &msg->relayed_addr) != 0) {
          return -1;
        }
        break;
      case STUN_ATTR_TYPE_XOR_PEER_ADDRESS:
        if (stun_msg_get_xor_mask(msg, mask) != 0 ||
            stun_get_mapped_address(value, attr_len, mask, &msg->peer_addr) !=
                0) {
          return -1;
        }
        break;
      case STUN_ATTR_TYPE_DATA: {
        if (attr_len > sizeof(msg->data)) {
          return -1;
        }
        memcpy(msg->data, value, attr_len);
        msg->data_len = attr_len;
        break;
      }
      case STUN_ATTR_TYPE_XOR_MAPPED_ADDRESS:
        if (stun_msg_get_xor_mask(msg, mask) != 0 ||
            stun_get_mapped_address(
                value, attr_len, mask, &msg->mapped_addr) != 0) {
          return -1;
        }
        break;
      case STUN_ATTR_TYPE_PRIORITY:
        break;
      case STUN_ATTR_TYPE_USE_CANDIDATE:
        break;
      case STUN_ATTR_TYPE_FINGERPRINT:
        if (attr_len != sizeof(msg->fingerprint)) {
          return -1;
        }
        msg->fingerprint = stun_read_u32(value);
        break;
      case STUN_ATTR_TYPE_ICE_CONTROLLED:
      case STUN_ATTR_TYPE_ICE_CONTROLLING:
      case STUN_ATTR_TYPE_NETWORK_COST:
        // Do nothing
        break;
      default:
        break;
      }

    pos += padded_len + STUN_ATTRIBUTE_HEADER_SIZE;
  }
  return 0;
}

void stun_calculate_fingerprint(
    const uint8_t* buf, size_t len, uint32_t* fingerprint) {
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0u; i < len; ++i) {
    c = CRC32_TABLE[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
  }
  *fingerprint = (c ^ 0xFFFFFFFF) ^ STUN_FINGERPRINT_XOR;
}

int stun_msg_write_attr(
    StunMessage* msg, StunAttrType type, uint16_t length,
    const void* value) {
  size_t padded_length = 4u * ((length + 3u) / 4u);
  if (msg == NULL || msg->size < STUN_HEADER_SIZE ||
      msg->size > sizeof(msg->buf) || (value == NULL && length != 0u) ||
      padded_length + STUN_ATTRIBUTE_HEADER_SIZE >
          sizeof(msg->buf) - msg->size) {
    return -1;
  }
  uint16_t header_length = stun_read_u16(msg->buf + 2u);
  size_t added_length = STUN_ATTRIBUTE_HEADER_SIZE + padded_length;
  if (added_length > UINT16_MAX - header_length) {
    return -1;
  }

  uint8_t* attribute = msg->buf + msg->size;
  stun_write_u16(attribute, (uint16_t)type);
  stun_write_u16(attribute + 2u, length);
  memset(attribute + STUN_ATTRIBUTE_HEADER_SIZE, 0, padded_length);
  if (value != NULL) {
    memcpy(attribute + STUN_ATTRIBUTE_HEADER_SIZE, value, length);
  }
  stun_write_u16(
      msg->buf + 2u, (uint16_t)(header_length + added_length));
  msg->size += added_length;

  switch (type) {
    case STUN_ATTR_TYPE_REALM:
      stun_copy_text_attr(
          msg->realm, sizeof(msg->realm), (const char*)value, length);
      break;
    case STUN_ATTR_TYPE_NONCE:
      stun_copy_text_attr(
          msg->nonce, sizeof(msg->nonce), (const char*)value, length);
      break;
    case STUN_ATTR_TYPE_USERNAME:
      stun_copy_text_attr(
          msg->username, sizeof(msg->username), (const char*)value, length);
      break;
    default:
      break;
  }

  return 0;
}

int stun_msg_finish(const h2_pal_crypto_api_t* crypto,
                    StunMessage* msg, StunCredential credential,
                    const char* password, size_t password_len) {
  if (msg == NULL || password == NULL || msg->size < STUN_HEADER_SIZE ||
      msg->size > sizeof(msg->buf) ||
      sizeof(msg->buf) - msg->size < 32u) {
    return -1;
  }
  uint16_t header_length = stun_read_u16(msg->buf + 2u);
  if (header_length > UINT16_MAX - 32u) {
    return -1;
  }
  char key[256];
  char hash_key[17];
  memset(key, 0, sizeof(key));
  memset(hash_key, 0, sizeof(hash_key));

  switch (credential) {
    case STUN_CREDENTIAL_LONG_TERM:
      if (snprintf(key, sizeof(key), "%s:%s:%s", msg->username, msg->realm,
                   password) >= (int)sizeof(key)) {
        return -1;
      }
      if (utils_get_md5(crypto, key, strlen(key), (unsigned char *)hash_key) !=
          H2_PAL_OK) {
        return -1;
      }
      password = hash_key;
      password_len = 16;
      break;
    default:
      break;
  }

  uint8_t* attribute = msg->buf + msg->size;
  stun_write_u16(
      msg->buf + 2u,
      (uint16_t)(header_length + STUN_ATTRIBUTE_HEADER_SIZE + 20u));
  stun_write_u16(attribute, STUN_ATTR_TYPE_MESSAGE_INTEGRITY);
  stun_write_u16(attribute + 2u, 20u);
  if (utils_get_hmac_sha1(
          crypto, (char*)msg->buf, msg->size, password, password_len,
          attribute + STUN_ATTRIBUTE_HEADER_SIZE) != H2_PAL_OK) {
    return -1;
  }
  msg->size += STUN_ATTRIBUTE_HEADER_SIZE + 20u;

  attribute = msg->buf + msg->size;
  stun_write_u16(msg->buf + 2u, (uint16_t)(header_length + 32u));
  stun_write_u16(attribute, STUN_ATTR_TYPE_FINGERPRINT);
  stun_write_u16(attribute + 2u, 4u);
  uint32_t fingerprint = 0u;
  stun_calculate_fingerprint(msg->buf, msg->size, &fingerprint);
  stun_write_u32(attribute + STUN_ATTRIBUTE_HEADER_SIZE, fingerprint);
  msg->size += STUN_ATTRIBUTE_HEADER_SIZE + 4u;
  return 0;
}

int stun_probe(const uint8_t *buf, size_t size) {
  if (buf == NULL || size < STUN_HEADER_SIZE) {
    return -1;
  }
  if (stun_read_u32(buf + 4u) != MAGIC_COOKIE) {
    return -1;
  }
  return 0;
}
int stun_msg_is_valid(const h2_pal_crypto_api_t* crypto,
                      const uint8_t* buf, size_t size,
                      const char* password) {
  StunMessage msg;

  if (buf == NULL || password == NULL || size < STUN_HEADER_SIZE + 32u ||
      size > sizeof(msg.buf)) {
    return -1;
  }

  memcpy(msg.buf, buf, size);
  msg.size = size;

  if (stun_parse_msg_buf(&msg) != 0) {
    return -1;
  }

  uint32_t fingerprint = 0;
  size_t length = size - STUN_ATTRIBUTE_HEADER_SIZE - 4u;
  stun_calculate_fingerprint(msg.buf, length, &fingerprint);

  if (fingerprint != msg.fingerprint) {
    return -1;
  }

  unsigned char message_integrity[20];
  uint16_t header_length = stun_read_u16(msg.buf + 2u);
  if (header_length < 32u) {
    return -1;
  }
  stun_write_u16(msg.buf + 2u, (uint16_t)(header_length - 8u));
  length -= 20u + STUN_ATTRIBUTE_HEADER_SIZE;
  if (utils_get_hmac_sha1(
          crypto, (char*)msg.buf, length, password, strlen(password),
          message_integrity) != H2_PAL_OK) {
    return -1;
  }

  if (memcmp(message_integrity, msg.message_integrity, 20) != 0) {
    return -1;
  }
  return 0;
}
