include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(H2_COMPONENT_SOURCES ap ap)

set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
if("${REPO_ROOT}" STREQUAL "")
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../../../.." ABSOLUTE)
endif()

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "."
    "${REPO_ROOT}/projects/testing/benchmark/webrtc-performance/app/include"
    "${REPO_ROOT}/projects/h2loader/libs/device_service/include"
    "${REPO_ROOT}/libs/h2loader/device/include"
    "${REPO_ROOT}/native_component_src/bk7258/ap/h2_pal_core/include"
    "${REPO_ROOT}/native_component_src/bk7258/ap/h2_pal_core/src"
    "${REPO_ROOT}/pal/include"
    "${REPO_ROOT}/libs/runtime/include"
  REQUIRES
    h2_bk_target_task_policy
    h2_bk7258_board
    bk_cli
    h2_firmware_lib
    h2_pal_core
    driver
    media_service
    multimedia
    bk_display
    bk_peripheral)

# Registration returns early during the SDK's script-mode dependency scan.
# Only the unchanged benchmark translation unit uses the target's UART
# formatter. SDK, libc and all other libraries retain their normal stdio.
foreach(H2_SOURCE IN LISTS H2_COMPONENT_SOURCES)
  get_filename_component(H2_SOURCE_NAME "${H2_SOURCE}" NAME)
  if(H2_SOURCE_NAME STREQUAL "h2_webrtc_performance.c")
    set_source_files_properties("${H2_SOURCE}" PROPERTIES COMPILE_DEFINITIONS
      "printf=h2_zero_bk_webrtc_perf_printf;puts=h2_zero_bk_webrtc_perf_puts")
    set_property(SOURCE "${H2_SOURCE}" APPEND PROPERTY COMPILE_OPTIONS
      "-include" "${CMAKE_CURRENT_LIST_DIR}/h2_bk_webrtc_perf_hooks.h")
  endif()
endforeach()

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c11
    -Wall
    -Wextra
    -Werror
    -Wno-unused-parameter
    -Wno-type-limits)
