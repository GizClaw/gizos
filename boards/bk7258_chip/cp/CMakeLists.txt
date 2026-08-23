include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_BOARD_SOURCES h2_bk7258_chip_cp cp)

armino_component_register(
  SRCS ${H2_BOARD_SOURCES}
  INCLUDE_DIRS include)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=c11 -Wall -Wextra -Werror)
