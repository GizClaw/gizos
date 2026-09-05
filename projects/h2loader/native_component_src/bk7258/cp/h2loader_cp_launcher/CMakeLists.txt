include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES cp cp)

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "."
  PRIV_REQUIRES
    h2_bk_target_task_policy
    bk_cli
    bk_rtos
    h2_cp_transport
    driver
    bk_pm)
