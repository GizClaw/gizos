if(NOT DEFINED H2_REPO_ROOT)
  get_filename_component(H2_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../.." ABSOLUTE)
else()
  get_filename_component(H2_REPO_ROOT "${H2_REPO_ROOT}" ABSOLUTE)
endif()

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)

idf_component_register(
  SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS
    "${H2_REPO_ROOT}/projects/e2e/apps/iperf-server/app/include"
    "${H2_REPO_ROOT}/libs/iperf/include"
    "${H2_REPO_ROOT}/libs/atomic/include"
    "${H2_REPO_ROOT}/libs/pal/include"
    "${H2_REPO_ROOT}/libs/runtime/include"
    "${H2_REPO_ROOT}/native_component_src/esp-idf6.x/h2_pal_core/include"
    "${H2_REPO_ROOT}/projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_runtime/include"
  REQUIRES
    h2_esp_target_task_policy
    esp_netif
    lwip
    esp_timer
    esp_hw_support
    app_update
    espcoredump
    esp_driver_uart
    esp_driver_usb_serial_jtag
    h2_firmware_lib
    h2_esp_board
    h2_h2loader_ble
    h2_h2loader_runtime
    h2_pal_core)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)

# Device qualification uses the same controller with an explicit boot mode.
if(DEFINED H2_IPERF_SERVER_AUTOSTART_MODE)
  if(NOT H2_IPERF_SERVER_AUTOSTART_MODE MATCHES "^(0|4|6|46)$")
    message(FATAL_ERROR "H2_IPERF_SERVER_AUTOSTART_MODE must be 0, 4, 6 or 46")
  endif()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_IPERF_SERVER_AUTOSTART_MODE=${H2_IPERF_SERVER_AUTOSTART_MODE})
endif()
