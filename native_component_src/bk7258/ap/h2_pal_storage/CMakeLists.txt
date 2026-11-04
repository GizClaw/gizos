set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
include("${REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_pal_storage ap)
armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS "include" "${REPO_ROOT}/libs/pal/include"
  PRIV_REQUIRES bk_common bk_rtos driver fatfs)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter)
