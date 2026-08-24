set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
set(H2_LVGL_ROOT "${REPO_ROOT}/libs/lvgl")
set(H2_LVGL_CONFIG_ROOT "${REPO_ROOT}/third_party/lvgl_patch/config/bk7258")

armino_component_register(
    INCLUDE_DIRS
        "${H2_LVGL_CONFIG_ROOT}"
        "${H2_LVGL_ROOT}/include"
        "${REPO_ROOT}/libs/pal/include"
    REQUIRES h2_firmware_lib
    PRIV_REQUIRES h2_pal_core bk_common bk_rtos)

target_compile_definitions(${COMPONENT_LIB} INTERFACE
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE)
