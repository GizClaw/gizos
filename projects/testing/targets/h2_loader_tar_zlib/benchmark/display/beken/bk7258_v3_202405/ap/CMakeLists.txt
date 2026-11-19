include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES ap ap)

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS "$ENV{H2_GIZOS_ROOT}/projects/testing/benchmark/display"
  REQUIRES h2_bk_target_task_policy h2_bk7258_board h2_firmware_lib h2_pal_core
           driver)

file(STRINGS "$ENV{H2_FIRMWARE_VERSION_FILE}" H2_BENCH_VERSION LIMIT_COUNT 1)
target_compile_definitions(${COMPONENT_LIB} PRIVATE H2_BENCH_VERSION="${H2_BENCH_VERSION}")
target_compile_options(${COMPONENT_LIB} PRIVATE -std=c11 -Wall -Wextra -Werror
                                              -Wno-unused-parameter -Wno-type-limits)
