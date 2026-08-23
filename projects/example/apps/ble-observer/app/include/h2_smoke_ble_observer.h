#ifndef H2_SMOKE_BLE_OBSERVER_H
#define H2_SMOKE_BLE_OBSERVER_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Scan for and record portable BLE advertising reports. */
int h2_smoke_ble_observer_run(h2_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
