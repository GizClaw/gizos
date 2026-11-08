include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES ap ap)
set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "."
    "${REPO_ROOT}/projects/testing/targets/h2_loader_tar_zlib/mfg/haivivi"
    "${REPO_ROOT}/projects/testing/mfg/app/include"
    "${REPO_ROOT}/libs/h2loader/device/include"
    "${REPO_ROOT}/libs/runtime/include"
    "${REPO_ROOT}/pal/include"
    "${REPO_ROOT}/boards/haivivi/h106_zero_bk_2_0/bk7258/ap/include"
    "${REPO_ROOT}/native_component_src/bk7258/ap/h2_pal_core/include"
  REQUIRES
    h2_bk_target_task_policy
    h2_bk7258_board
    h2_firmware_lib
    h2_pal_core
    bk_common
    bk_rtos
    driver
    zlib)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=c11 -Wall -Wextra -Werror)
