#ifndef H2_GIZCLAW_E2E_CATALOG_H
#define H2_GIZCLAW_E2E_CATALOG_H

#include "h2_gizclaw_e2e_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct e2e_case {
  const char *id;
  uint32_t suite;
  size_t actor_count;
  bool needs_voice;
  int (*run)(h2_gizclaw_e2e_fixture_t *fixture);
} e2e_case_t;

/* Link exactly one catalog. The full app retains all six acceptance cases;
 * a dedicated connectivity app is a measurement lane, never full acceptance. */
extern const e2e_case_t h2_gizclaw_e2e_cases[];
extern const size_t h2_gizclaw_e2e_case_count;

#ifdef __cplusplus
}
#endif
#endif
