#ifndef H2_H202_BK7258_BOARD_H
#define H2_H202_BK7258_BOARD_H

#include "os/h2_pal_fs.h"
#include <stdbool.h>
struct h2_runtime_config;

#define H2_BK7258_DL_MOUNT_PATH "/dl"
#define H2_BK7258_DATA_MOUNT_PATH "/data"

/** @brief Configure safe peripheral rails before starting management workers.
 * Boot-owner only; repeatable. Returns H2_PAL_ERR_IO on GPIO failure. */
int h2_bk7258_board_init(void);
/** @brief Borrow a stable factory-MAC-derived name, or NULL on read failure.
 * Called by the boot owner; returned storage remains valid for device lifetime.
 */
const char* h2_bk7258_board_ble_local_name(void);
#define H2_BK7258_BOARD_BLE_LOCAL_NAME h2_bk7258_board_ble_local_name
/** Publish the SD filesystem before startup. Mounting is explicit via prepare.
 */
int h2_bk7258_board_fs_init(h2_pal_fs_api_t* fs);
/** Read the card socket switch. Call after board initialization. */
bool h2_bk7258_board_sd_present(void);
/** Qualify the inserted SD card before starting MFG. May format an inserted
 * card with no recognized filesystem. The caller serializes filesystem use. */
int h2_bk7258_board_sd_check(void);
/** Release the SD volume after filesystem users have stopped. */
int h2_bk7258_board_fs_deinit(void);
/** Before MFG Runtime setup, qualify a GPIO deep-sleep wake by a continuous
 * power-key hold. Early release requests deep sleep again; normal boots and
 * software resets proceed immediately. A returning sleep request is an error.
 */
int h2_bk7258_board_wait_for_wakeup(uint32_t hold_ms);
/** Populate MFG Runtime dependencies after task-policy installation. The
 * config borrows board providers until board_runtime_deinit succeeds. Does
 * not create Runtime or run tests. An NFC initialization error leaves the
 * corresponding capability unavailable for an explicit failed MFG result. */
int h2_bk7258_board_runtime_config(struct h2_runtime_config* config);
/** After every Runtime/App/management user has stopped, release board-owned
 * providers. Failure retains unfinished ownership for a later retry. */
int h2_bk7258_board_runtime_deinit(void);

#endif
