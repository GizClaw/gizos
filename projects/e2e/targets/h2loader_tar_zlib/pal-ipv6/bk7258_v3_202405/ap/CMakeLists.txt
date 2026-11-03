include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES ap ap)

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "."
    "$ENV{H2_GIZOS_ROOT}/projects/e2e/libs/pal-ipv6-device/include"
    "$ENV{H2_GIZOS_ROOT}/projects/h2loader/native_component_src/bk7258/ap/h2loader_bk/include"
    "$ENV{H2_GIZOS_ROOT}/native_component_src/bk7258/ap/h2_pal_core/include"
  REQUIRES h2_bk7258_board h2_bk_target_task_policy h2_firmware_lib h2loader_bk h2_pal_core)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=c11 -Wall -Wextra -Werror)
