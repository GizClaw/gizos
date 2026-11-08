include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(PAL_DISK_SOURCES ap ap)
armino_component_register(
  SRCS ${PAL_DISK_SOURCES}
  INCLUDE_DIRS "." ${H2_BAZEL_COMPONENT_INCLUDES_AP_H2_BK7258_BOARD}
  REQUIRES h2_bk7258_board h2_bk_target_task_policy h2_firmware_lib
    h2_pal_core h2_pal_image h2_pal_storage driver bk_rtos)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=c11 -Wall -Wextra -Werror)
