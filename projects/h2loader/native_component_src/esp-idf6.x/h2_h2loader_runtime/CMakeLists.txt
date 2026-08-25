get_filename_component(H2_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_H2LOADER_RUNTIME_SRCS h2_h2loader_runtime)

idf_component_register(
  SRCS ${H2_H2LOADER_RUNTIME_SRCS}
  INCLUDE_DIRS
    "include"
    "${H2_REPO_ROOT}/projects/h2loader/apps/loader/app/include"
    "${H2_REPO_ROOT}/projects/h2loader/libs/h2loader/include"
    "${H2_REPO_ROOT}/libs/command/include"
    "${H2_REPO_ROOT}/libs/trie/include"
    "${H2_REPO_ROOT}/libs/pal/include"
    "${H2_REPO_ROOT}/libs/runtime/include"
  REQUIRES
    app_update
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_rom
    h2_firmware_lib
    h2_pal_core
    mbedtls)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
