include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_DIAGNOSTIC_SOURCES h2_mosaico_usb_diagnostic)
idf_component_register(SRCS ${H2_DIAGNOSTIC_SOURCES}
  INCLUDE_DIRS "$ENV{H2_GIZOS_ROOT}/boards/esp_mosaico/esp32s31/include"
  REQUIRES h2_mosaico_usb_console h2_pal_core espressif__esp_tinyusb esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
