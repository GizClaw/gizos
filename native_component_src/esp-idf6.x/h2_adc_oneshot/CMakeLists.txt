include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_adc_oneshot)

idf_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
  REQUIRES
    esp_adc
    h2_firmware_lib
    freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
