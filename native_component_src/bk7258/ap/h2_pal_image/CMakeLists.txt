set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
include("${REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_pal_image ap)
armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS "include" "${REPO_ROOT}/libs/pal/include"
    "${REPO_ROOT}/native_component_src/bk7258/shared/fixed_flash"
  PRIV_REQUIRES bk_common bk_rtos driver ota h2_pal_storage)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter)
