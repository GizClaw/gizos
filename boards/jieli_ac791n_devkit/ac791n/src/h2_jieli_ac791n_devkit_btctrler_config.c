/* Compile JieLi's complete controller configuration into every firmware that
 * uses this board. Keep the vendor BLE demo's controller feature selection;
 * in particular, legacy pairing does not require the optional SC submodule. */
#if defined(H2_JIELI_BLE_DIAG_CONTROLLER_STATIC_RAM) && \
    !defined(CONFIG_NO_SDRAM_ENABLE)
/* JieLi couples the controller's internal LE RAM mode to the application's
 * SDRAM setting.  The official BLE-only example selects the non-SDRAM branch.
 * Allow a Loader-only diagnostic to select that controller setting without
 * lying to the rest of the firmware about the board's usable SDRAM. */
#define CONFIG_NO_SDRAM_ENABLE
#define H2_JIELI_BLE_DIAG_UNDEFINE_NO_SDRAM
#endif
#include "config/log_config/lib_btctrler_config.c"
#ifdef H2_JIELI_BLE_DIAG_UNDEFINE_NO_SDRAM
#undef H2_JIELI_BLE_DIAG_UNDEFINE_NO_SDRAM
#undef CONFIG_NO_SDRAM_ENABLE
#endif
