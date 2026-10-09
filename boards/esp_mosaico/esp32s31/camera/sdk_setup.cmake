# Include after the native component manifest and before ESP-IDF project.cmake.
# The caller must select the optional board camera component in its Bazel graph.
include_guard(GLOBAL)
if(NOT H2_BAZEL_COMPONENT_SRCS_MOSAICO_MODULE_CAMERA)
  message(FATAL_ERROR "Select //boards/esp_mosaico/esp32s31:camera before camera setup")
endif()
# Override only esp_driver_cam in this build; never patch the shared SDK.
list(GET H2_BAZEL_COMPONENT_SRCS_MOSAICO_MODULE_CAMERA 0 H2_CAMERA_SOURCE)
get_filename_component(H2_CAMERA_VENDOR "${H2_CAMERA_SOURCE}" DIRECTORY)
set(H2_CAMERA_SDK_COPY "${CMAKE_BINARY_DIR}/camera-sdk")
file(REMOVE_RECURSE "${H2_CAMERA_SDK_COPY}")
file(MAKE_DIRECTORY "${H2_CAMERA_SDK_COPY}/components")
file(COPY "$ENV{IDF_PATH}/components/esp_driver_cam" DESTINATION "${H2_CAMERA_SDK_COPY}/components")
find_program(H2_CAMERA_GIT git REQUIRED)
execute_process(COMMAND "${H2_CAMERA_GIT}" -C "${H2_CAMERA_SDK_COPY}" apply --no-index
  "${H2_CAMERA_VENDOR}/patches/esp-idf-dvp-frame-capture-stability.patch"
  RESULT_VARIABLE H2_CAMERA_PATCH_RC ERROR_VARIABLE H2_CAMERA_PATCH_ERROR)
if(NOT H2_CAMERA_PATCH_RC EQUAL 0)
  message(FATAL_ERROR "DVP stability patch failed: ${H2_CAMERA_PATCH_ERROR}")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${H2_CAMERA_SDK_COPY}/components/esp_driver_cam")
