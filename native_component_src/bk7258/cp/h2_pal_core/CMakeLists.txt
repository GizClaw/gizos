set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
set(H2_BK_PLATFORM_CORE_PRIV_REQUIRES
  bk_common
  bk_rtos)
include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_pal_core cp)

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
    "${REPO_ROOT}/libs/pal/include"
  PRIV_REQUIRES
    ${H2_BK_PLATFORM_CORE_PRIV_REQUIRES})
