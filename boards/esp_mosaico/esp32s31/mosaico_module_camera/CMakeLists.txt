include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_VENDOR_SOURCES mosaico_module_camera)
list(GET H2_VENDOR_SOURCES 0 H2_VENDOR_SOURCE)
get_filename_component(H2_VENDOR_DIR "${H2_VENDOR_SOURCE}" DIRECTORY)
idf_component_register(SRCS ${H2_VENDOR_SOURCES} INCLUDE_DIRS "${H2_VENDOR_DIR}/include"
  REQUIRES mosaico_module_mgr esp_mosaico_bsp espressif__esp_video esp_driver_cam esp_driver_jpeg esp_driver_ppa esp_hw_support)

# The pinned BSP requires these upstream fixes. Do not silently ship without them.
find_program(H2_GIT git REQUIRED)
function(h2_camera_patch source patch)
  execute_process(COMMAND "${H2_GIT}" -C "${source}" apply --no-index --reverse --check "${patch}"
    RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
  if(NOT applied EQUAL 0)
    execute_process(COMMAND "${H2_GIT}" -C "${source}" apply --no-index "${patch}"
      RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Camera stability patch failed: ${error}")
    endif()
  endif()
endfunction()
h2_camera_patch("${PROJECT_DIR}" "${H2_VENDOR_DIR}/patches/esp-cam-sensor-sc101iot-pclk-delay.patch")
