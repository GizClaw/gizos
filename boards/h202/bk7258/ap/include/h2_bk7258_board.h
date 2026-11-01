#ifndef H2_H202_BK7258_BOARD_H
#define H2_H202_BK7258_BOARD_H

#include "h2/pal/os/h2_pal_fs.h"
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
/** @brief Publish the lazy filesystem API without mounting or formatting.
 * @param fs Caller-owned output; must not be NULL. No retained output pointer.
 * @return H2_PAL_OK, or H2_PAL_ERR_INVALID_ARG for NULL.
 * The boot owner must first configure rails with h2_bk7258_board_init().
 * Calls through the API are serialized by the Loader operation owner.
 * mkdir/open/stat can mount and block on IO without reconfiguring board GPIOs;
 * mount before starting concurrent users. Borrowed API remains valid until
 * exit.
 */
int h2_bk7258_board_fs_init(h2_pal_fs_api_t* fs);
/** @brief Unmount storage after all users and open files have stopped.
 * @return H2_PAL_OK (including repeat calls), or H2_PAL_ERR_IO.
 * SDK unmount consumes the mount even on error; subsequent use remounts.
 */
int h2_bk7258_board_fs_deinit(void);
/** Populate MFG Runtime dependencies after task-policy installation. The
 * config borrows board providers until board_runtime_deinit succeeds. Does
 * not create Runtime or run tests. NFC/modem initialization errors leave the
 * corresponding capability unavailable for an explicit failed MFG result. */
int h2_bk7258_board_runtime_config(struct h2_runtime_config* config);
/** After every Runtime/App/management user has stopped, release board-owned
 * providers. Failure retains unfinished ownership for a later retry. */
int h2_bk7258_board_runtime_deinit(void);

#endif
