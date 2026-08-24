set(H2_ESP_ES8311_ES7210_REQUIRES
  esp_driver_gpio
  esp_driver_i2c
  esp_driver_i2s
  esp-sr
  h2_firmware_lib)

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_es8311_es7210_audio_system)

idf_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
  PRIV_INCLUDE_DIRS
    "src"
  REQUIRES
    ${H2_ESP_ES8311_ES7210_REQUIRES}
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
