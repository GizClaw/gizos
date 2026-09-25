#ifndef H2_DESKTOP_ARENA_DIAGNOSTICS_PROBE_H
#define H2_DESKTOP_ARENA_DIAGNOSTICS_PROBE_H

#include "h2_mem_arena_diagnostics.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kernel-assisted copy and provider-owned stack capture. These callbacks
 * remain valid for the process lifetime and have no associated user state. */
h2_mem_arena_diagnostics_probe_t h2_desktop_arena_diagnostics_probe(void);

#ifdef __cplusplus
}
#endif
#endif
