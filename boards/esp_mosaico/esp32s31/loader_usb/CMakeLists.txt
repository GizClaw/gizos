include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_LOADER_USB_SOURCES h2_mosaico_loader_usb)
idf_component_register(SRCS ${H2_LOADER_USB_SOURCES}
  INCLUDE_DIRS "$ENV{H2_GIZOS_ROOT}/boards/esp_mosaico/esp32s31/include"
  REQUIRES h2_mosaico_usb_console h2_h2loader_runtime espressif__esp_tinyusb esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
