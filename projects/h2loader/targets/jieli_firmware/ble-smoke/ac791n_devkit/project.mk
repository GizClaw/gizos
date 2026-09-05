H2_JIELI_DIAG_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
H2_GIZOS_ROOT := $(abspath $(H2_JIELI_DIAG_ROOT)/../../../../../..)
H2_JIELI_LAYOUT_ROOT := $(H2_JIELI_DIAG_ROOT)
H2_JIELI_SDRAM_ENABLE := 0
H2_JIELI_RESERVED_EXPAND_CONFIG_FILE := $(H2_GIZOS_ROOT)/boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/isd_reserved_expand.ini

# Match apps/demo/demo_ble/board/wl82/Makefile.  DOUBLE_BANK is the only
# additional define and keeps this bounded test recoverable by H2Loader.
H2_JIELI_BOARD_DEFINES := \
	-DCONFIG_BT_ENABLE=1 \
	-DCONFIG_TWS_ENABLE \
	-DCONFIG_BTCTRLER_TASK_DEL_ENABLE \
	-DCONFIG_LMP_CONN_SUSPEND_ENABLE \
	-DCONFIG_LMP_REFRESH_ENCRYPTION_KEY_ENABLE \
	-DCONFIG_DOUBLE_BANK_ENABLE=1

H2_JIELI_BOARD_LIBS := \
	cpu/wl82/liba/wl_rf_common.a \
	cpu/wl82/liba/btctrler.a \
	cpu/wl82/liba/btstack.a \
	cpu/wl82/liba/crypto_toolbox_Osize.a \
	cpu/wl82/liba/lib_ccm_aes.a \
	cpu/wl82/liba/lib_sig_mesh.a

H2_JIELI_BOARD_INCLUDES := \
	-Iapps/demo/demo_ble/include \
	-Iapps/common \
	-Iapps/common/include \
	-Iapps/common/config/include \
	-Iapps/common/ble/include \
	-Iinclude_lib/btstack \
	-Iinclude_lib/net \
	-Iinclude_lib/net/lwip_2_2_0/lwip/src/include \
	-Iinclude_lib/net/lwip_2_2_0/lwip/src/include/lwip \
	-Iinclude_lib/net/lwip_2_2_0/lwip/port \
	-Iinclude_lib/net/lwip_2_2_0/lwip/app \
	-Iinclude_lib/utils/btmesh \
	-Iinclude_lib/utils/btmesh/adaptation

H2_JIELI_BOARD_C_SRC_FILES := \
	apps/common/ble/le_24g_deal.c \
	apps/common/ble/le_hogp.c \
	apps/common/ble/le_net_central.c \
	apps/common/ble/le_net_cfg.c \
	apps/common/ble/le_net_cfg_dui.c \
	apps/common/ble/le_net_cfg_qyai.c \
	apps/common/ble/le_net_cfg_tencent.c \
	apps/common/ble/le_net_cfg_turing.c \
	apps/common/ble/le_trans_data.c \
	apps/common/ble/mesh/examples/AliGenie_fan.c \
	apps/common/ble/mesh/examples/AliGenie_light.c \
	apps/common/ble/mesh/examples/AliGenie_socket.c \
	apps/common/ble/mesh/examples/TUYA_light.c \
	apps/common/ble/mesh/examples/generic_onoff_client.c \
	apps/common/ble/mesh/examples/generic_onoff_server.c \
	apps/common/ble/mesh/examples/onoff_tobe_provision.c \
	apps/common/ble/mesh/examples/provisioner.c \
	apps/common/ble/mesh/examples/vendor_client.c \
	apps/common/ble/mesh/examples/vendor_server.c \
	apps/common/ble/mesh/mesh_config_common.c \
	apps/common/ble/mesh/model_api.c \
	apps/common/ble/mesh/unix_timestamp.c \
	apps/common/ble/multi_demo/le_multi_client.c \
	apps/common/ble/multi_demo/le_multi_common.c \
	apps/common/ble/multi_demo/le_multi_trans.c \
	apps/common/config/bt_profile_config.c \
	apps/common/config/log_config/app_config.c \
	apps/common/config/log_config/lib_btctrler_config.c \
	apps/common/config/log_config/lib_btstack_config.c \
	apps/common/net/wifi_conf.c \
	apps/demo/demo_ble/board/wl82/board.c \
	apps/demo/demo_ble/bt_ble/ble.c

include $(H2_GIZOS_ROOT)/boards/ac791n_chip/ac791n/layouts/compile_only/project.mk
