include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_CAMERA_SOURCES h2_mosaico_camera)
idf_component_register(SRCS ${H2_CAMERA_SOURCES}
  INCLUDE_DIRS "$ENV{H2_GIZOS_ROOT}/boards/esp_mosaico/esp32s31/include" "$ENV{H2_GIZOS_ROOT}/libs/pal/include"
  REQUIRES mosaico_module_camera)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
