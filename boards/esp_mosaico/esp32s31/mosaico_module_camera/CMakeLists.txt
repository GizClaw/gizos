include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_VENDOR_SOURCES mosaico_module_camera)
list(GET H2_VENDOR_SOURCES 0 H2_VENDOR_SOURCE)
get_filename_component(H2_VENDOR_DIR "${H2_VENDOR_SOURCE}" DIRECTORY)
idf_component_register(SRCS ${H2_VENDOR_SOURCES} INCLUDE_DIRS "${H2_VENDOR_DIR}/include"
  REQUIRES mosaico_module_mgr esp_mosaico_bsp espressif__esp_video esp_driver_cam esp_driver_jpeg esp_driver_ppa esp_hw_support)
