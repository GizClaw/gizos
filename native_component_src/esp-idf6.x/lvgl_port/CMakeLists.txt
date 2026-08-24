set(REPO_ROOT "${H2_REPO_ROOT}")
set(H2_LVGL_ROOT "${REPO_ROOT}/libs/lvgl")
set(H2_LVGL_CONFIG_ROOT "${REPO_ROOT}/third_party/lvgl_patch/config/esp_idf6_x")

idf_component_register(
    INCLUDE_DIRS
        "${H2_LVGL_CONFIG_ROOT}"
        "${H2_LVGL_ROOT}/include"
    REQUIRES
      h2_firmware_lib
        h2_pal_core
    PRIV_INCLUDE_DIRS
        "private_include"
    PRIV_REQUIRES
        esp_timer
        freertos
        log)

if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()

target_compile_definitions(${COMPONENT_LIB} INTERFACE
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE)
