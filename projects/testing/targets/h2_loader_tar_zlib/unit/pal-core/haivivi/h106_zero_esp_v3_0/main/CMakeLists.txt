include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)
set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")

idf_component_register(
  SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS
    "${REPO_ROOT}/projects/testing/unit/pal-core/app/include"
    "${REPO_ROOT}/libs/sync/atomic/include"
    "${REPO_ROOT}/pal/include"
    "${REPO_ROOT}/libs/runtime/include"
    "${REPO_ROOT}/native_component_src/esp-idf6.x/h2_pal_core/include"
  REQUIRES
    h2_esp_target_task_policy
    app_update
    espcoredump
    esp_driver_uart
    h2_firmware_lib
    h2_esp_board
    h2_pal_core)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
# Preserve the board's arena selection and its one-time Task configuration.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=h2_esp_platform_task_configure")
