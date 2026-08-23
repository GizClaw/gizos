include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_es8311_audio_system)

idf_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
  REQUIRES
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    h2_firmware_lib
    esp-sr
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
