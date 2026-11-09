#ifndef H2_MFG_PEER_H
#define H2_MFG_PEER_H
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Only probes the explicit target UID, never accepts manual MFG checks. */
int h2_mfg_peer_run(h2_runtime_t* runtime, const char* uid,
                    uint32_t timeout_ms);
/* P4's C6 radio address can differ from its authoritative P4 factory UID. */
int h2_mfg_peer_run_with_address(h2_runtime_t* runtime, const char* uid,
                                 const char* ble_address, uint32_t timeout_ms);
const char* h2_mfg_peer_dut_uid(void);
const char* h2_mfg_peer_dut_ble_address(void);
#ifdef __cplusplus
}
#endif
#endif
