#ifndef H2_LIBSRTP_CONFIG_H
#define H2_LIBSRTP_CONFIG_H

#include <limits.h>
#include <stdint.h>

_Static_assert(
    sizeof(uint32_t) == sizeof(unsigned int),
    "libSRTP 2.8 private cipher lengths require 32-bit unsigned int");

/*
 * libSRTP 2.8.0's private cipher vtable uses unsigned int lengths while its
 * dispatch functions use uint32_t. Some 32-bit targets typedef uint32_t as
 * unsigned long. Both representations have the same ABI on supported targets;
 * the package build disables strict aliasing and downgrades the resulting
 * pointer and debug-format warnings without changing either public headers or
 * platform integer definitions.
 */

#define GCM 1
#define HAVE_NETINET_IN_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UINT8_T 1
#define HAVE_UINT16_T 1
#define HAVE_UINT32_T 1
#define HAVE_UINT64_T 1
#define HAVE_INT8_T 1
#define HAVE_INT16_T 1
#define HAVE_INT32_T 1
#define HAVE_INTTYPES_H 1
#if ULONG_MAX == 0xffffffffUL
#define SIZEOF_UNSIGNED_LONG 4
#elif ULONG_MAX == 0xffffffffffffffffUL
#define SIZEOF_UNSIGNED_LONG 8
#else
#error "Unsupported unsigned long width"
#endif

#if ULLONG_MAX == 0xffffffffULL
#define SIZEOF_UNSIGNED_LONG_LONG 4
#elif ULLONG_MAX == 0xffffffffffffffffULL
#define SIZEOF_UNSIGNED_LONG_LONG 8
#else
#error "Unsupported unsigned long long width"
#endif
#define PACKAGE_NAME "libsrtp2"
#define PACKAGE_STRING "libsrtp2 2.8.0"
#define PACKAGE_VERSION "2.8.0"

/* cipher_types.h only declares this module for bundled crypto engines. */
#include "err.h"
extern srtp_debug_module_t srtp_mod_aes_gcm;

#endif
