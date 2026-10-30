if(DEFINED H2_ESP_BOARD_DIR)
  get_filename_component(H2_REPO_ROOT "${H2_ESP_BOARD_DIR}/../../.." ABSOLUTE)
else()
  get_filename_component(H2_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_BOARD_SOURCES h2_esp_board)

idf_component_register(
  SRCS ${H2_BOARD_SOURCES}
  INCLUDE_DIRS
    "include"
    "${H2_REPO_ROOT}/libs/pal/include"
    "${H2_REPO_ROOT}/libs/runtime/include"
  REQUIRES
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_spi
    esp_driver_tsens
    esp_lcd_co5300
    esp_lcd_touch_cst9217
    esp_partition
    esp_timer
    h2_es8311_es7210_audio_system
    h2_firmware_lib
    h2_pal_core
    joltwallet__littlefs)

target_compile_definitions(${COMPONENT_LIB} PRIVATE H2_WS175C_BOARD_ENABLE_QMI8658=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
