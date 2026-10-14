include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_board_application_host)
idf_component_register(SRCS ${H2_COMPONENT_SOURCES} INCLUDE_DIRS "include" REQUIRES h2_esp_board h2_application_host)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
