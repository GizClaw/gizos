include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_USB_SOURCES h2_mosaico_loader_usb)
idf_component_register(SRCS ${H2_USB_SOURCES}
  INCLUDE_DIRS "include"
  REQUIRES h2_mosaico_usb h2_h2loader_runtime h2_firmware_lib)
