include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES opus_port ap)
h2_bazel_native_component_select_sources(OPUS_ROOT_MARKER opus_port ap src/opus.c)
list(GET OPUS_ROOT_MARKER 0 OPUS_ROOT_MARKER)
get_filename_component(OPUS_ROOT_SRC "${OPUS_ROOT_MARKER}" DIRECTORY)
get_filename_component(OPUS_ROOT "${OPUS_ROOT_SRC}" DIRECTORY)

armino_component_register(
    SRCS ${H2_COMPONENT_SOURCES}
    INCLUDE_DIRS "${OPUS_ROOT}/include"
    PRIV_INCLUDE_DIRS
        "${OPUS_ROOT}"
        "${OPUS_ROOT}/celt"
        "${OPUS_ROOT}/silk"
        "${OPUS_ROOT}/silk/fixed"
        "${OPUS_ROOT}/src")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OPUS_BUILD
    FIXED_POINT
    DISABLE_FLOAT_API
    HAVE_LRINT
    HAVE_LRINTF
    HAVE_MEMORY_H
    VAR_ARRAYS)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -Wno-unused-function
    -Wno-unused-variable)
