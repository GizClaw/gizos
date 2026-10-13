include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_application_host ap)
armino_component_register(SRCS ${H2_COMPONENT_SOURCES} INCLUDE_DIRS "include" REQUIRES h2_firmware_lib)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
