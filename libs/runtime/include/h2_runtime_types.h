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
typedef uint64_t h2_runtime_sequence_t;
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
