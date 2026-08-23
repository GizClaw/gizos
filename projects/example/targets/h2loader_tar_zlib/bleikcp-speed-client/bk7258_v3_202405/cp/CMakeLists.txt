include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES cp cp)

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "."
  PRIV_REQUIRES
    bk_cli
    bk_rtos
    h2_cp_transport
    driver
    bk_pm)
