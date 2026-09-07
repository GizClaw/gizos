#ifndef H2_RUNTIME_TYPES_H
#define H2_RUNTIME_TYPES_H

/*
 * Scope: Common runtime types shared by events, state, and input acquisition.
 * Keep this header free of component-specific payload details.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_runtime h2_runtime_t;

typedef uint32_t h2_runtime_id_t;
/*
 * Event sequence. Distinguishes and orders events over a window, not for the
 * lifetime of the device: the counter wraps at UINT32_MAX (skipping 0, which
 * means "no sequence") and consumers compare with h2_runtime_sequence_after().
 * 32 bits keep it a single lock-free atomic add on every core that has one.
 */
typedef uint32_t h2_runtime_sequence_t;

/*
 * Wrap-safe "a was issued after b"; a == b is not after. 0 is "no sequence",
 * so any real sequence is after it and nothing is after a real sequence
 * when compared with 0 on the left.
 */
static inline int h2_runtime_sequence_after(
    h2_runtime_sequence_t a,
    h2_runtime_sequence_t b) {
    if (a == 0u || b == 0u) {
        return a != 0u && b == 0u;
    }
    return (int32_t)(a - b) > 0;
}
typedef uint64_t h2_runtime_timestamp_ms_t;

typedef struct h2_runtime_string {
    /* Byte span; data is not guaranteed to be NUL-terminated. */
    const char *data;
    size_t len;
} h2_runtime_string_t;

#ifdef __cplusplus
}
#endif

#endif
