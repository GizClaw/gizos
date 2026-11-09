#ifndef H2_GIZCLAW_BLE_PEER_H
#define H2_GIZCLAW_BLE_PEER_H
#include "h2_runtime.h"
/* This fixture is the wireless central. It reads only the explicit BLE UID,
 * never binds an account, and never publishes credential bytes. */
int h2_gizclaw_ble_peer_run(h2_runtime_t* runtime, const char* dut_uid,
                            const char* origin, unsigned transfers);
const char* h2_ble_peer_dut_uid(void);
const char* h2_ble_peer_origin(void);
#endif
