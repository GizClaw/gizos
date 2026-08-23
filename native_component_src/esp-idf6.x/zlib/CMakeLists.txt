set(ZLIB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/zlib")

include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(ZLIB_SOURCES zlib)

idf_component_register(
    SRCS ${ZLIB_SOURCES}
    INCLUDE_DIRS "${ZLIB_ROOT}")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-implicit-fallthrough
    -Wno-unused-function)
