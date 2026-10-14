include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_pal_e2e_application_host ap)
armino_component_register(SRCS ${H2_COMPONENT_SOURCES} INCLUDE_DIRS "$ENV{H2_GIZOS_ROOT}/projects/e2e/libs/application-host/include" REQUIRES h2_board_application_host h2_bk_target_task_policy h2_pal_core h2loader_bk h2_firmware_lib)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
