#ifndef H2_LIBSRTP_NETINET_IN_H
#define H2_LIBSRTP_NETINET_IN_H

#if !defined(_WIN32) && !defined(H2_LIBSRTP_NO_SYSTEM_NETINET)

/* The private include root is propagated by Bazel, so do not shadow POSIX. */
#include_next <netinet/in.h>

#else

#include <stdint.h>

/*
 * libSRTP uses netinet/in.h only for host/network byte-order conversion.
 * Keep that private upstream include portable on Windows and embedded targets
 * without pulling a socket API into the PAL-backed package.
 */

static inline uint16_t h2_libsrtp_network_u16(uint16_t value) {
    return (uint16_t)((value >> 8u) | (value << 8u));
}

static inline uint32_t h2_libsrtp_network_u32(uint32_t value) {
    return ((value & UINT32_C(0x000000ff)) << 24u) |
           ((value & UINT32_C(0x0000ff00)) << 8u) |
           ((value & UINT32_C(0x00ff0000)) >> 8u) |
           ((value & UINT32_C(0xff000000)) >> 24u);
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

#endif
