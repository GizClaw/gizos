include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_BOARD_SOURCES h2_esp_board)
idf_component_register(
  SRCS ${H2_BOARD_SOURCES}
  INCLUDE_DIRS include
  REQUIRES h2_es8311_audio_system espressif__esp_lcd_touch h2_chip_board esp_mosaico_bsp h2_pal_core h2_firmware_lib joltwallet__littlefs)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
