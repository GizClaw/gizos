#ifndef H2_SMOKE_BLE_ADVERTISING_H
#define H2_SMOKE_BLE_ADVERTISING_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Run the portable legacy and Extended Advertising smoke sequence. */
int h2_smoke_ble_advertising_run(h2_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
