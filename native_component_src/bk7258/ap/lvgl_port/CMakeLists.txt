set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
set(H2_LVGL_ROOT "${REPO_ROOT}/libs/lvgl")
set(H2_LVGL_CONFIG_ROOT "${REPO_ROOT}/third_party/lvgl_patch/config/bk7258")

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES lvgl_port ap)

armino_component_register(
    SRCS ${H2_COMPONENT_SOURCES}
    INCLUDE_DIRS
        "."
        "${H2_LVGL_CONFIG_ROOT}"
        "${H2_LVGL_ROOT}/include"
        "${REPO_ROOT}/libs/pal/include"
    REQUIRES h2_firmware_lib
    PRIV_REQUIRES h2_pal_core bk_common bk_rtos driver)

target_compile_definitions(${COMPONENT_LIB} INTERFACE
    # Native consumers also inherit generic Bazel headers. Select the
    # canonical platform source independently of their include ordering.
    LV_CONF_PATH=\"${H2_LVGL_CONFIG_ROOT}/lv_conf.h\"
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE)

# Keep BK lifecycle policy out of portable LVGL and its consumers.
armino_build_set_property(LINK_OPTIONS "-Wl,--wrap=lv_deinit" APPEND)
