#ifndef H2_GIZCLAW_E2E_OUTPUT_H
#define H2_GIZCLAW_E2E_OUTPUT_H
#include "h2_gizclaw_e2e.h"
#ifdef __cplusplus
extern "C" {
#endif
void h2_gizclaw_e2e_set_evidence_observer(h2_gizclaw_e2e_evidence_fn observer,
                                       void *user);
int h2_gizclaw_e2e_emit(const char *format, ...);
#ifdef __cplusplus
}
#endif
#endif
