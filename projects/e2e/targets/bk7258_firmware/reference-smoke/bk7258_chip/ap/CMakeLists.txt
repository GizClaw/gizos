include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main ap)

armino_component_register(
  SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS
    "$ENV{H2_GIZOS_ROOT}/boards/bk7258_chip/ap/include"
    "$ENV{H2_GIZOS_ROOT}/projects/e2e/libs/reference_smoke/include"
  REQUIRES h2_bk7258_chip_ap h2_firmware_lib)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=c11 -Wall -Wextra -Werror)
