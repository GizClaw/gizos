include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_es8311_audio_system)

# ESP-SR's pinned prebuilt libraries do not supply an S31 ABI. The shared
# codec/mixer implementation remains usable with AEC explicitly disabled.
set(H2_ES8311_SR_REQUIRES)
if(NOT IDF_TARGET STREQUAL "esp32s31")
  list(APPEND H2_ES8311_SR_REQUIRES esp-sr)
endif()

idf_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
  REQUIRES
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    h2_firmware_lib
    ${H2_ES8311_SR_REQUIRES}
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
