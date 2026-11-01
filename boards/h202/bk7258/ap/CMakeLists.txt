include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_bk7258_board ap)
set(H2_H202_BOARD_REQUIRES h2_pal_core h2_firmware_lib littlefs bk_vfs)
if(CONFIG_AUDIO_DMIC)
  list(APPEND H2_H202_BOARD_REQUIRES
    h2_nfc_fm175xx audio_pipeline onboard_dual_dmic_mic_stream
    onboard_speaker_stream media_service lwip_intf_v2_1)
endif()
armino_component_register_ex(CONFIG_COMPONENT_NAME h2_bk7258_board
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS "$ENV{H2_GIZOS_ROOT}/boards/h202/bk7258/include" "include"
  PRIV_REQUIRES ${H2_H202_BOARD_REQUIRES})
