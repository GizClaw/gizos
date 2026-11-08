include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)
set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")

idf_component_register(
  SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS
    "${REPO_ROOT}/projects/testing/targets/h2_loader_tar_zlib/mfg/haivivi"
    "${REPO_ROOT}/projects/testing/mfg/app/include"
    "${REPO_ROOT}/libs/h2loader/device/include"
    "${REPO_ROOT}/libs/runtime/include"
    "${REPO_ROOT}/pal/include"
  REQUIRES h2_esp_target_task_policy h2_firmware_lib h2_esp_board h2_pal_core)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
