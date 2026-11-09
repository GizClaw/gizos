include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)

idf_component_register(
  SRCS ${H2_MAIN_SOURCES}
  REQUIRES h2_pal_core h2_firmware_lib h2_esp_target_task_policy)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
