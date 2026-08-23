include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main cp)

armino_component_register(
  SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS "$ENV{H2_REPO_ROOT}/boards/bk7258_chip/cp/include")

target_compile_options(${COMPONENT_LIB} PRIVATE -std=c11 -Wall -Wextra -Werror)
