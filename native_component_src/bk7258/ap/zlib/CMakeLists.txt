include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES zlib ap)
list(GET H2_COMPONENT_SOURCES 0 ZLIB_FIRST_SOURCE)
get_filename_component(ZLIB_ROOT "${ZLIB_FIRST_SOURCE}" DIRECTORY)

armino_component_register(
    SRCS ${H2_COMPONENT_SOURCES}
    INCLUDE_DIRS "${ZLIB_ROOT}")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function)
