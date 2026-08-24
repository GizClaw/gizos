include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES ap ap)

set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
if("${REPO_ROOT}" STREQUAL "")
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../../.." ABSOLUTE)
endif()

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "."
    "${REPO_ROOT}/projects/example/apps/wifi-csi/app/include"
    "${REPO_ROOT}/projects/h2loader/libs/h2loader/include"
    "${REPO_ROOT}/boards/bk7258_v3_202405/bk7258/ap/include"
    "${REPO_ROOT}/native_component_src/bk7258/ap/h2_pal_core/include"
    "${REPO_ROOT}/projects/h2loader/native_component_src/bk7258/ap/h2loader_bk/include"
    "${REPO_ROOT}/libs/pal/include"
    "${REPO_ROOT}/libs/runtime/include"
  REQUIRES
    h2_bk7258_board
    bk_cli
    h2loader_bk
    h2_pal_core
    driver
    media_service
    multimedia
    bk_display
    bk_peripheral)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c11
    -Wall
    -Wextra
    -Werror
    -Wno-unused-parameter
    -Wno-type-limits)
