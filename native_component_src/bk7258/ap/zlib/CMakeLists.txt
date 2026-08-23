set(ZLIB_ROOT "$ENV{H2_REPO_ROOT}/third_party/zlib")
include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES zlib ap)

armino_component_register(
    SRCS ${H2_COMPONENT_SOURCES}
    INCLUDE_DIRS "${ZLIB_ROOT}")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function)
