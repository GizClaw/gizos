#ifndef H2_GIZCLAW_E2E_BLE_BINDING_H
#define H2_GIZCLAW_E2E_BLE_BINDING_H

#include "h2_gizclaw_e2e_internal.h"

/* Uses the caller-started real BLE Host and a temporary server API key. A
 * central must complete two independent connections within the case budget. */
int h2_gizclaw_e2e_run_ble_binding(h2_gizclaw_e2e_fixture_t *fixture);

#endif
