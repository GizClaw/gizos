#ifndef H2_PAL_ERRORS_H
#define H2_PAL_ERRORS_H

#include <limits.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Signed 32-bit PAL result with the C int callback ABI.
 *
 * Providers and libraries may return negative domain-specific errors outside
 * the named PAL constants; every signed 32-bit value is preserved, including
 * when the target compiles enums with a compressed representation. All objects
 * exchanging PAL results must use this contract; it is not a wire format.
 */
typedef signed int h2_pal_result_t;
#ifdef __cplusplus
static_assert(sizeof(h2_pal_result_t) * CHAR_BIT == 32 &&
              INT_MIN == INT32_MIN && INT_MAX == INT32_MAX,
              "PAL results require a signed 32-bit C int ABI");
#else
/* Default MSVC C mode does not support C11 _Static_assert. */
typedef char h2_pal_result_requires_signed_32_bit_int[
    sizeof(h2_pal_result_t) * CHAR_BIT == 32 &&
    INT_MIN == INT32_MIN && INT_MAX == INT32_MAX ? 1 : -1];
#endif

enum h2_pal_result {
    H2_PAL_OK = 0,
    H2_PAL_EXIT = 1,
    H2_PAL_ERR_INVALID_ARG = -1,
    H2_PAL_ERR_UNAVAILABLE = -2,
    H2_PAL_ERR_UNSUPPORTED = -3,
    H2_PAL_ERR_IO = -4,
    H2_PAL_ERR_NO_MEMORY = -5,
    H2_PAL_ERR_TIMEOUT = -6,
    H2_PAL_ERR_INVALID_STATE = -7,
    H2_PAL_ERR_NOT_FOUND = -8,
    H2_PAL_ERR_WOULD_BLOCK = -9,
    H2_PAL_ERR_CLOSED = -10,
    H2_PAL_ERR_FULL = -11,
    H2_PAL_ERR_TASK = -12,
    H2_PAL_ERR_NO_SPACE = -13,
    H2_PAL_ERR_WRITE = -14,
    H2_PAL_ERR_FORMAT = -15,
    H2_PAL_ERR_TRUNCATED = -16,
    H2_PAL_ERR_TLS_VERIFY = -17,
    H2_PAL_ERR_BUSY = -18,
};

#define H2_AUDIO_OK H2_PAL_OK
#define H2_AUDIO_ERR_INVALID_ARG H2_PAL_ERR_INVALID_ARG
#define H2_AUDIO_ERR_UNAVAILABLE H2_PAL_ERR_UNAVAILABLE
#define H2_AUDIO_ERR_UNSUPPORTED H2_PAL_ERR_UNSUPPORTED
#define H2_AUDIO_ERR_IO H2_PAL_ERR_IO
#define H2_AUDIO_ERR_NO_MEMORY H2_PAL_ERR_NO_MEMORY
#define H2_AUDIO_ERR_INVALID_STATE H2_PAL_ERR_INVALID_STATE
#define H2_AUDIO_ERR_WOULD_BLOCK H2_PAL_ERR_WOULD_BLOCK

#define H2_DISPLAY_OK H2_PAL_OK
#define H2_DISPLAY_ERR_INVALID_ARG H2_PAL_ERR_INVALID_ARG
#define H2_DISPLAY_ERR_UNAVAILABLE H2_PAL_ERR_UNAVAILABLE
#define H2_DISPLAY_ERR_UNSUPPORTED H2_PAL_ERR_UNSUPPORTED
#define H2_DISPLAY_ERR_IO H2_PAL_ERR_IO
#define H2_DISPLAY_ERR_NO_MEMORY H2_PAL_ERR_NO_MEMORY
#define H2_DISPLAY_ERR_INVALID_STATE H2_PAL_ERR_INVALID_STATE

#define H2_PAL_FS_OK H2_PAL_OK
#define H2_PAL_FS_ERR_INVALID_ARG H2_PAL_ERR_INVALID_ARG
#define H2_PAL_FS_ERR_NO_SPACE H2_PAL_ERR_NO_SPACE
#define H2_PAL_FS_ERR_NO_MEMORY H2_PAL_ERR_NO_MEMORY
#define H2_PAL_FS_ERR_NOT_FOUND H2_PAL_ERR_NOT_FOUND
#define H2_PAL_FS_ERR_IO H2_PAL_ERR_IO
#define H2_PAL_FS_ERR_UNSUPPORTED H2_PAL_ERR_UNSUPPORTED

#define H2_PAL_LOG_OK H2_PAL_OK
#define H2_PAL_LOG_ERR_INVALID_ARG H2_PAL_ERR_INVALID_ARG
#define H2_PAL_LOG_ERR_WRITE H2_PAL_ERR_WRITE
#define H2_PAL_LOG_ERR_FORMAT H2_PAL_ERR_FORMAT
#define H2_PAL_LOG_ERR_TRUNCATED H2_PAL_ERR_TRUNCATED

#define H2_PAL_QUEUE_OK H2_PAL_OK
#define H2_PAL_QUEUE_ERR_INVALID_ARG H2_PAL_ERR_INVALID_ARG
#define H2_PAL_QUEUE_ERR_NO_MEMORY H2_PAL_ERR_NO_MEMORY
#define H2_PAL_QUEUE_ERR_TIMEOUT H2_PAL_ERR_TIMEOUT
#define H2_PAL_QUEUE_ERR_CLOSED H2_PAL_ERR_CLOSED
#define H2_PAL_QUEUE_ERR_IO H2_PAL_ERR_IO

#ifdef __cplusplus
}
#endif

#endif
