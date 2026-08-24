include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_bk7258_board ap)

armino_component_register_ex(CONFIG_COMPONENT_NAME h2_bk7258_board
    SRCS ${H2_COMPONENT_SOURCES}
    INCLUDE_DIRS
        "include"
    REQUIRES
        h2_firmware_lib
    PRIV_REQUIRES
        h2_firmware_lib
        h2_pal_core
        driver
        bk_wifi
        audio_play
        audio_record
        media_service
        multimedia
        bk_display
        bk_peripheral)
