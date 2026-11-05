include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_bk7258_board ap)

armino_component_register_ex(CONFIG_COMPONENT_NAME h2_bk7258_board
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "$ENV{H2_REPO_ROOT}/boards/haivivi/h106_zero_bk_1_0/bk7258/include"
    "include"
    "src"
  PRIV_REQUIRES
    audio_play
    audio_record
    h2_firmware_lib
    h2_nfc_fm175xx
    h2_pal_core
    h2_pal_image
    h2_pal_storage
    h2_firmware_lib
    littlefs
    lwip_intf_v2_1
    media_service)
