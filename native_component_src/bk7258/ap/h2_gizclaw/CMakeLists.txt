set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_gizclaw ap)

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "${REPO_ROOT}/libs/gizclaw/include"
  PRIV_REQUIRES
    h2_firmware_lib
    driver
    h2_pal_core
    opus_port)

target_compile_options(
  ${COMPONENT_LIB}
  PRIVATE
    -std=gnu11
    -Wall
    -Wextra
    -Werror
    -Wno-error=type-limits)
target_link_options(${COMPONENT_LIB} INTERFACE
  "-Wl,--undefined=h2_gizclaw_conversation_invalidate_internal"
  "-Wl,--undefined=gzc_default_platform")
