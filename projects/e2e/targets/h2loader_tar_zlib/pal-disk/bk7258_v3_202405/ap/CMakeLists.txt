include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(PAL_DISK_SOURCES ap ap)
set(PAL_DISK_INCLUDES
  "."
  ${H2_BAZEL_COMPONENT_INCLUDES_AP_H2_BK7258_BOARD})
armino_component_register(
  SRCS ${PAL_DISK_SOURCES}
  INCLUDE_DIRS ${PAL_DISK_INCLUDES}
  REQUIRES h2_bk7258_board h2_bk_target_task_policy h2_firmware_lib
    h2loader_bk h2_pal_core driver bk_rtos)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=c11 -Wall -Wextra -Werror)
