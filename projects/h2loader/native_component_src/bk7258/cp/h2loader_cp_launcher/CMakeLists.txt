include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES cp cp)

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "."
    "$ENV{H2_GIZOS_ROOT}/projects/h2loader/native_component_src/bk7258/fixed_flash"
    "$ENV{ARMINO_PATH}/middleware/soc/bk7258/hal"
  PRIV_REQUIRES
    h2_bk_target_task_policy
    h2_pal_core
    bk_cli
    bk_rtos
    driver
    bk_pm)
