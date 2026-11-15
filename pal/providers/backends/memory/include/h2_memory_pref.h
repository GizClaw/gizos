#ifndef H2_MEMORY_PREF_H
#define H2_MEMORY_PREF_H

#include "os/h2_pal_pref.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_memory_pref h2_memory_pref_t;

typedef struct h2_memory_pref_config {
  const h2_pal_mem_api_t* mem;
  size_t max_namespaces;
  size_t max_entries_per_namespace;
  size_t max_value_bytes;
  size_t max_handles;
} h2_memory_pref_config_t;

/** Create a bounded, volatile Pref provider. Borrows mem until destroy.
 * All calls, including calls on separate handles, must be serialized by the
 * caller. Names are limited to 64 bytes and keys to 96 bytes, excluding NUL.
 * A namespace permits one writer; readers see committed data, while the writer
 * sees staging. commit atomically replaces committed data in RAM only. It does
 * not write files or survive provider destruction/process or hardware reset.
 * Failed mutation/commit preserves the prior committed state. Closing a writer
 * discards its uncommitted changes. One metadata-snapshot iterator per handle
 * is allowed. All freed values are explicitly wiped.
 * Returns INVALID_ARG or NO_MEMORY; *out is NULL on failure. */
h2_pal_result_t h2_memory_pref_create(const h2_memory_pref_config_t* config,
                                      h2_memory_pref_t** out);

/** Borrow the capability until destroy. NULL owner returns NULL. */
const h2_pal_pref_api_t* h2_memory_pref_api(h2_memory_pref_t* owner);

/** Quiescent-only. Open handles return INVALID_STATE without freeing anything.
 * Close each iterator before its namespace. Success wipes/frees all values and
 * clears *owner. NULL/repeated destroy is harmless. */
h2_pal_result_t h2_memory_pref_destroy(h2_memory_pref_t** owner);

#ifdef __cplusplus
}
#endif
#endif
