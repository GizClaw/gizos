#include "stun.h"

// These tests use assertions for both checks and the operations under test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

static void write_u16(uint8_t out[2], uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void test_parse_bounds(void) {
    StunMessage message;
    memset(&message, 0, sizeof(message));
    assert(stun_parse_msg_buf(&message) != 0);

    stun_msg_create(&message, STUN_METHOD_BINDING);
    assert(stun_parse_msg_buf(&message) == 0);

    write_u16(message.buf + 2u, 4u);
    assert(stun_parse_msg_buf(&message) != 0);

    memset(&message, 0, sizeof(message));
    stun_msg_create(&message, STUN_METHOD_BINDING);
    write_u16(message.buf + STUN_HEADER_SIZE, STUN_ATTR_TYPE_USERNAME);
    write_u16(message.buf + STUN_HEADER_SIZE + 2u, UINT16_MAX);
    write_u16(message.buf + 2u, 4u);
    message.size += 4u;
    assert(stun_parse_msg_buf(&message) != 0);
}

static void test_address_round_trip(h2_pal_net_family_t family) {
    static const uint8_t transaction_id[STUN_TRANSACTION_ID_SIZE] = {
        0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u,
        0x16u, 0x17u, 0x18u, 0x19u, 0x1au, 0x1bu,
    };
    StunMessage message;
    h2_pal_net_addr_t address;
    uint8_t mask[16];
    uint8_t encoded[20];
    memset(&message, 0, sizeof(message));
    memset(&address, 0, sizeof(address));
    address.family = family;
    address.port = 0x1234u;
    size_t ip_len = family == H2_PAL_NET_FAMILY_IPV6 ? 16u : 4u;
    for (size_t i = 0u; i < ip_len; ++i) {
        address.ip[i] = (uint8_t)(0x20u + i);
    }

    stun_msg_create(&message, (uint16_t)STUN_CLASS_RESPONSE |
                                  (uint16_t)STUN_METHOD_BINDING);
    stun_msg_set_transaction_id(&message, transaction_id);
    assert(stun_msg_get_xor_mask(&message, mask) == 0);
    int encoded_len = stun_set_mapped_address(
        encoded, sizeof(encoded), mask, &address);
    assert(encoded_len == (family == H2_PAL_NET_FAMILY_IPV6 ? 20 : 8));
    assert(encoded[0] == 0u);
    assert(encoded[1] == (family == H2_PAL_NET_FAMILY_IPV6
                              ? STUN_FAMILY_IPV6
                              : STUN_FAMILY_IPV4));
    assert(encoded[2] == 0x33u);
    assert(encoded[3] == 0x26u);
    assert(encoded[4] == (uint8_t)(address.ip[0] ^ 0x21u));
    assert(encoded[5] == (uint8_t)(address.ip[1] ^ 0x12u));
    assert(stun_msg_write_attr(
               &message,
               STUN_ATTR_TYPE_XOR_MAPPED_ADDRESS,
               (uint16_t)encoded_len,
               encoded) == 0);
    assert(stun_parse_msg_buf(&message) == 0);
    assert(message.mapped_addr.family == address.family);
    assert(message.mapped_addr.port == address.port);
    assert(memcmp(message.mapped_addr.ip, address.ip, ip_len) == 0);
}

static void test_address_rejection(void) {
    static const uint8_t mask[16] = {0};
    uint8_t value[20] = {0};
    h2_pal_net_addr_t address;
    memset(&address, 0xa5, sizeof(address));

    value[1] = 3u;
    assert(stun_get_mapped_address(
               value, 8u, mask, &address) != 0);
    assert(address.family == 0);
    assert(address.port == 0u);

    value[1] = STUN_FAMILY_IPV4;
    assert(stun_get_mapped_address(
               value, 7u, mask, &address) != 0);
    value[1] = STUN_FAMILY_IPV6;
    assert(stun_get_mapped_address(
               value, 19u, mask, &address) != 0);

    memset(&address, 0, sizeof(address));
    address.family = (h2_pal_net_family_t)5;
    assert(stun_set_mapped_address(
               value, sizeof(value), mask, &address) != 0);
    address.family = H2_PAL_NET_FAMILY_IPV6;
    assert(stun_set_mapped_address(
               value, 19u, mask, &address) != 0);
}

static void test_write_and_finish_bounds(void) {
    StunMessage message;
    memset(&message, 0, sizeof(message));
    stun_msg_create(&message, STUN_METHOD_BINDING);
    char value = 'x';
    assert(stun_msg_write_attr(
               &message,
               STUN_ATTR_TYPE_USERNAME,
               UINT16_MAX,
               &value) != 0);

    message.size = sizeof(message.buf) - 31u;
    assert(stun_msg_finish(
               NULL,
               &message,
               STUN_CREDENTIAL_SHORT_TERM,
               "password",
               sizeof("password") - 1u) != 0);
}

int main(void) {
    test_parse_bounds();
    test_address_round_trip(H2_PAL_NET_FAMILY_IPV4);
    test_address_round_trip(H2_PAL_NET_FAMILY_IPV6);
    test_address_rejection();
    test_write_and_finish_bounds();
    return 0;
}
