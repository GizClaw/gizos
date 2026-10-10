include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_CONSOLE_SOURCES h2_mosaico_usb_console)
idf_component_register(SRCS ${H2_CONSOLE_SOURCES}
  INCLUDE_DIRS "$ENV{H2_GIZOS_ROOT}/boards/esp_mosaico/esp32s31/include"
  REQUIRES espressif__esp_tinyusb)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
