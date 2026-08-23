set(REPO_ROOT "$ENV{H2_REPO_ROOT}")

set(H2_BK_H2LOADER_SRCS
  "src/h2_bk_h2loader_app_control.c"
  "src/h2_bk_h2loader_app_bleikcp.c"
  "src/h2_bk_h2loader_iostreamikcp.c"
  "src/h2_bk_h2loader_task_policy.c"
  "src/h2_bk_h2loader_ota_writer.c"
  "src/h2_bk_h2loader_power.c"
  "${REPO_ROOT}/projects/h2loader/libs/h2loader/src/h2_loader_app_client.c"
  "${REPO_ROOT}/projects/h2loader/libs/h2loader/src/h2_loader_ble.c"
  "${REPO_ROOT}/projects/h2loader/libs/h2loader/src/h2_loader_boot.c"
  "${REPO_ROOT}/projects/h2loader/libs/h2loader/src/h2_loader_status.c")
set(H2_BK_H2LOADER_PRIV_REQUIRES
  bk_common
  bk_cli
  bk_rtos
  driver
  h2_firmware_lib
  h2_pal_core
  h2_firmware_lib
  ota)

if(NOT DEFINED H2_BK_H2LOADER_ENABLE_FATFS)
  set(H2_BK_H2LOADER_ENABLE_FATFS ON)
endif()
if(H2_BK_H2LOADER_ENABLE_FATFS)
  list(APPEND H2_BK_H2LOADER_SRCS "src/h2_bk_h2loader_fs.c")
  list(APPEND H2_BK_H2LOADER_PRIV_REQUIRES fatfs)
else()
  list(APPEND H2_BK_H2LOADER_SRCS "src/h2_bk_h2loader_storage_none.c")
endif()
include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_select_sources(
  H2_COMPONENT_SOURCES
  h2loader_bk
  ap
  ${H2_BK_H2LOADER_SRCS})

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
    "${REPO_ROOT}/projects/h2loader/libs/h2loader/include"
    "${REPO_ROOT}/libs/bundle/include"
    "${REPO_ROOT}/libs/command/include"
    "${REPO_ROOT}/libs/iostreamikcp/include"
    "${REPO_ROOT}/libs/pal/include"
  PRIV_REQUIRES ${H2_BK_H2LOADER_PRIV_REQUIRES})

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c11
    -Wall
    -Wextra
    -Werror
    -Wno-unused-parameter)
