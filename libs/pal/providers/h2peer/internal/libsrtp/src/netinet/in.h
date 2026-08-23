#ifndef H2_LIBSRTP_NETINET_IN_H
#define H2_LIBSRTP_NETINET_IN_H

#include <stdint.h>

/*
 * libSRTP uses netinet/in.h only for host/network byte-order conversion.
 * Keep that private upstream include portable without pulling a socket API
 * into the PAL-backed package.
 */

static inline uint16_t h2_libsrtp_network_u16(uint16_t value) {
#if defined(_WIN32)
    return (uint16_t)((value >> 8u) | (value << 8u));
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return value;
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(value);
#else
#error "Unsupported byte order"
#endif
}

static inline uint32_t h2_libsrtp_network_u32(uint32_t value) {
#if defined(_WIN32)
    return ((value & UINT32_C(0x000000ff)) << 24u) |
           ((value & UINT32_C(0x0000ff00)) << 8u) |
           ((value & UINT32_C(0x00ff0000)) >> 8u) |
           ((value & UINT32_C(0xff000000)) >> 24u);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return value;
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(value);
#else
#error "Unsupported byte order"
#endif
}

#ifndef htons
#define htons(value) h2_libsrtp_network_u16((uint16_t)(value))
#endif
#ifndef ntohs
#define ntohs(value) h2_libsrtp_network_u16((uint16_t)(value))
#endif
#ifndef htonl
#define htonl(value) h2_libsrtp_network_u32((uint32_t)(value))
#endif
#ifndef ntohl
#define ntohl(value) h2_libsrtp_network_u32((uint32_t)(value))
#endif

#endif
