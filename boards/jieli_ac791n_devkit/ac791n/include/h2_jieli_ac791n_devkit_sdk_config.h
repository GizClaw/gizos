#ifndef H2_JIELI_AC791N_DEVKIT_SDK_CONFIG_H
#define H2_JIELI_AC791N_DEVKIT_SDK_CONFIG_H

/* Physical board and boot/update configuration shared by every firmware role. */
#define __FLASH_SIZE__ (8 * 1024 * 1024)

#define TCFG_LOWPOWER_BTOSC_DISABLE 0
/* Match JieLi's physical WL82 multimedia DevKit reference configuration.
 * Individual layouts may override this before including the shared board
 * configuration for focused power/timing diagnostics. */
#ifndef TCFG_LOWPOWER_LOWPOWER_SEL
#define TCFG_LOWPOWER_LOWPOWER_SEL RF_SLEEP_EN
#endif
#define TCFG_LOWPOWER_VDDIOM_LEVEL VDDIOM_VOL_32V
#ifdef CONFIG_RTC_ENABLE
#define TCFG_LOWPOWER_VDDIOW_LEVEL VDDIOW_VOL_32V
#else
#define TCFG_LOWPOWER_VDDIOW_LEVEL VDDIOW_VOL_21V
#endif
#define VDC14_VOL_SEL_LEVEL VDC14_VOL_SEL_140V
#define SYSVDD_VOL_SEL_LEVEL SYSVDD_VOL_SEL_126V

#define CONFIG_CXX_SUPPORT
#define CONFIG_RF_TRIM_CODE_AT_RAM
#define CONFIG_DEBUG_ENABLE
#define CONFIG_DOUBLE_BANK_ENABLE 1
/* Use the SDK's 4-second hardware watchdog.  A stalled App must reset back to
 * Loader crash recovery instead of leaving USB and the display wedged. */
#define CONFIG_H2_WATCHDOG_ENABLE 1

#define TCFG_USER_BLE_ENABLE 1
#define TCFG_USER_BT_CLASSIC_ENABLE 0
#define CONFIG_BT_RX_BUFF_SIZE 0
#define CONFIG_BT_TX_BUFF_SIZE 0

#define TCFG_USER_EMITTER_ENABLE 0
#define TRANS_DATA_EN 1
#define TRANS_MULTI_BLE_EN 0
#define TRANS_MULTI_BLE_SLAVE_NUMS 0
#define TRANS_MULTI_BLE_MASTER_NUMS 0
#define BT_NET_CENTRAL_EN 0
#define BT_NET_HID_EN 0
#define CONFIG_BLE_MESH_ENABLE 0
#define APP_NONCONN_24G 0
/* H2Loader uses an unpaired GATT transport in both Loader and App roles. */
#define TCFG_BLE_SECURITY_EN 0
#define RCSP_MODE 0
#if !defined(CONFIG_DEBUG_ENABLE) || defined(CONFIG_LIB_DEBUG_DISABLE)
#define LIB_DEBUG 0
#else
#define LIB_DEBUG 1
#endif
#define CONFIG_DEBUG_LIB(value) ((value) & LIB_DEBUG)

#define TCFG_PC_ENABLE 1
#ifndef USB_PC_NO_APP_MODE
#define USB_PC_NO_APP_MODE 2
#endif
#define USB_MALLOC_ENABLE 1
#define USB_DEVICE_CLASS_CONFIG (CDC_CLASS)
#define TCFG_HOST_AUDIO_ENABLE 0
#define TCFG_HOST_UVC_ENABLE 0
#define TCFG_HID_HOST_ENABLE 0
#define TCFG_UDISK_ENABLE 0

#define CONFIG_SD_ENABLE 1
#define TCFG_SD0_ENABLE 1
#define TCFG_SD_PORTS 'A'
#define TCFG_SD_DAT_WIDTH 1
#define TCFG_SD_DET_MODE SD_CMD_DECT
#define TCFG_SD_DET_IO_LEVEL 0
#define TCFG_SD_CLK 20000000

#include "usb_std_class_def.h"
#include "usb_common_def.h"

#endif
