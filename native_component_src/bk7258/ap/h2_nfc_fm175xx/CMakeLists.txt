set(REPO_ROOT "$ENV{H2_REPO_ROOT}")
include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES h2_nfc_fm175xx ap)

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
    "${REPO_ROOT}/libs/drivers/nfc/fm175xx/include"
    "${REPO_ROOT}/libs/pal/include"
  REQUIRES
    h2_firmware_lib
    driver)

target_compile_options(
  ${COMPONENT_LIB}
  PRIVATE -std=c11 -Wall -Wextra -Werror)
