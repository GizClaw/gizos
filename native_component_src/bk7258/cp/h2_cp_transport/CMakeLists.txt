include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_cp_transport cp)

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
  PRIV_REQUIRES
    bk_cli
    bk_rtos
    driver)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "$ENV{ARMINO_PATH}/components/bk_cli")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c11
    -Wall
    -Wextra
    -Werror
    -Wno-type-limits)
