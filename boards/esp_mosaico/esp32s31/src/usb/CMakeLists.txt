include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_USB_SOURCES h2_mosaico_usb)
idf_component_register(SRCS ${H2_USB_SOURCES}
  INCLUDE_DIRS "$ENV{H2_GIZOS_ROOT}/boards/esp_mosaico/esp32s31/include"
  REQUIRES espressif__esp_tinyusb esp_timer vfs freertos h2_firmware_lib)
