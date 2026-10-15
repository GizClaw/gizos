#ifndef H2_MOBILE_E2E_RUNTIME_CONFIG_H
#define H2_MOBILE_E2E_RUNTIME_CONFIG_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Assemble borrowed packaged mobile PALs for an E2E entry. This factory owns no
 * App/Runtime lifecycle; provider getters keep their acquisition contracts.
 * The entry/suite runner owns Runtime and provider shutdown. */
h2_runtime_config_t h2_mobile_e2e_runtime_config(void);
#ifdef __cplusplus
}
#endif
#endif
