# Include after the native component manifest and before ESP-IDF project.cmake.
# The caller must select the optional board camera component in its Bazel graph.
include_guard(GLOBAL)
if(NOT H2_BAZEL_COMPONENT_SRCS_MOSAICO_MODULE_CAMERA)
  message(FATAL_ERROR "Select //boards/esp_mosaico/esp32s31:camera before camera setup")
endif()
# Use the pinned SDK driver without copying or patching its implementation.
# Any capture failure must remain visible to the board diagnostic and be
# reported as an SDK/provider issue rather than repaired in a build hook.
