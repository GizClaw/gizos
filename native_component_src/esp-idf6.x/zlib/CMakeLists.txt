include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(ZLIB_SOURCES zlib)
list(GET ZLIB_SOURCES 0 ZLIB_FIRST_SOURCE)
get_filename_component(ZLIB_ROOT "${ZLIB_FIRST_SOURCE}" DIRECTORY)

idf_component_register(
    SRCS ${ZLIB_SOURCES}
    INCLUDE_DIRS "${ZLIB_ROOT}")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-implicit-fallthrough
    -Wno-unused-function)
