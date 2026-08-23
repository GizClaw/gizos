get_filename_component(H2_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_H2LOADER_BLE_SRCS h2_h2loader_ble)

idf_component_register(
  SRCS ${H2_H2LOADER_BLE_SRCS}
  INCLUDE_DIRS "include"
  PRIV_INCLUDE_DIRS
    "${H2_REPO_ROOT}/projects/h2loader/apps/loader/app"
  REQUIRES
    h2_firmware_lib
    h2_h2loader_runtime)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
