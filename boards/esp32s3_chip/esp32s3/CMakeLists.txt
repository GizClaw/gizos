include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_BOARD_SOURCES h2_chip_board)

idf_component_register(
  SRCS ${H2_BOARD_SOURCES}
  INCLUDE_DIRS include)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
