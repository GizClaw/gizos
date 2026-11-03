include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_BOARD_SOURCES h2_esp_board)
idf_component_register(
  SRCS ${H2_BOARD_SOURCES}
  INCLUDE_DIRS include
  REQUIRES h2_pal_core h2_adc_oneshot h2_es8311_audio_system h2_firmware_lib
    esp_lcd esp_driver_spi esp_driver_ledc esp_driver_i2c esp_partition
)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
