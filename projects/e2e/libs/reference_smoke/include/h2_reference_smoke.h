#ifndef H2_REFERENCE_SMOKE_H
#define H2_REFERENCE_SMOKE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Returns a deterministic value used to prove a portable archive was linked. */
uint32_t h2_reference_smoke_value(uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif
