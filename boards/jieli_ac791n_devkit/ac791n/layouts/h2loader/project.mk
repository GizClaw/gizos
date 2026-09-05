H2_JIELI_LAYOUT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
H2_JIELI_SDRAM_ENABLE := 1
H2_JIELI_RESERVED_EXPAND_CONFIG_FILE := $(H2_JIELI_LAYOUT_ROOT)/isd_reserved_expand.ini
H2_JIELI_BOARD_DEFINES := \
	-DCONFIG_USB_ENABLE=1 \
	-DCONFIG_NET_ENABLE=1 \
	-DCONFIG_WIFI_ENABLE=1 \
	-DCONFIG_BT_ENABLE=1 \
	-DCONFIG_TWS_ENABLE \
	-DCONFIG_BTCTRLER_TASK_DEL_ENABLE \
	-DCONFIG_LMP_CONN_SUSPEND_ENABLE \
	-DCONFIG_LMP_REFRESH_ENCRYPTION_KEY_ENABLE \
	-DCONFIG_DOUBLE_BANK_ENABLE=1 \
	-DCONFIG_SAVE_EXCEPTION_LOG_IN_FLASH=1 \
	-DH2_JIELI_BLE_ENABLE=1 \
	-DH2_JIELI_NETWORK_ENABLE=1 \
	-DDISABLE_FLOAT_API \
	-DFIXED_POINT \
	-DHAVE_LRINT \
	-DHAVE_LRINTF \
	-DOPUS_BUILD \
	-DVAR_ARRAYS \
	-DJL_LWIP=1
H2_JIELI_BOARD_LIBS := \
	cpu/wl82/liba/audio_server.a \
	cpu/wl82/liba/wl_rf_common.a \
	cpu/wl82/liba/btctrler.a \
	cpu/wl82/liba/btstack.a \
	cpu/wl82/liba/crypto_toolbox_Osize.a \
	cpu/wl82/liba/lib_ccm_aes.a \
	cpu/wl82/liba/media_app.a \
	cpu/wl82/liba/libjlsp.a \
	cpu/wl82/liba/libaec.a \
	cpu/wl82/liba/libdns.a \
	cpu/wl82/liba/hsm.a \
	cpu/wl82/liba/wl_wifi.a \
	cpu/wl82/liba/hostapd_and_wpasupplicant.a \
	cpu/wl82/liba/wpasupplicant.a \
	cpu/wl82/liba/lwip_2_2_0.a
H2_JIELI_BOARD_INCLUDES := \
	-Iapps/common \
	-Iapps/common/include \
	-Iapps/common/config/include \
	-Iapps/common/example/third_party/littlefs \
	-Iapps/common/usb \
	-Iapps/common/usb/include \
	-Iapps/common/usb/include/host \
	-Iapps/common/usb/device \
	-Iapps/common/net \
	-Iinclude_lib/net \
	-Iinclude_lib/net/hostapdandwpa_supplicant \
	-Iinclude_lib/net/lwip_2_2_0/lwip/src/include \
	-Iinclude_lib/net/lwip_2_2_0/lwip/src/include/lwip \
	-Iinclude_lib/net/lwip_2_2_0/lwip/port \

H2_JIELI_BOARD_C_SRC_FILES := \
	apps/common/config/bt_profile_config.c \
	apps/common/config/log_config/app_config.c \
	apps/common/config/log_config/lib_btstack_config.c \
	apps/common/audio_music/audio_config.c \
	apps/common/audio_music/pcm_play_api.c \
	apps/common/jl_math/flfft_core_pi32v2.c \
	apps/common/jl_math/jl_fft.c \
	apps/common/jl_math/jl_math.c \
	apps/common/example/third_party/littlefs/lfs.c \
	apps/common/example/third_party/littlefs/lfs_util.c \
	apps/common/iic/iic.c \
	apps/common/iic/software_iic.c \
	apps/common/net/assign_macaddr.c \
	apps/common/net/config_network.c \
	apps/common/net/platform_cfg.c \
	apps/common/net/wifi_conf.c \
	apps/common/update/net_update.c \
	apps/common/usb/device/cdc.c \
	apps/common/usb/device/descriptor.c \
	apps/common/usb/device/msd_upgrade.c \
	apps/common/usb/device/usb_device.c \
	apps/common/usb/device/user_setup.c \
	apps/common/usb/usb_config.c \
	apps/common/usb/usb_epbuf_manager.c \

include $(abspath $(H2_JIELI_LAYOUT_ROOT)/../../../../ac791n_chip/ac791n/layouts/compile_only/project.mk)

LFLAGS += -T $(H2_JIELI_LAYOUT_ROOT)/sdk_abi.ld
