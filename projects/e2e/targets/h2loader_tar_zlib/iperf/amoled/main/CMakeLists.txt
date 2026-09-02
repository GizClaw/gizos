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
    "${H2_REPO_ROOT}/projects/e2e/apps/iperf/app/include"
    "${H2_REPO_ROOT}/libs/iperf/include"
    "${H2_REPO_ROOT}/libs/pal/include"
    "${H2_REPO_ROOT}/libs/pal/providers/h2sctp/include"
    "${H2_REPO_ROOT}/libs/runtime/include"
    "${H2_REPO_ROOT}/projects/h2loader/libs/h2loader/include"
  REQUIRES
    h2_esp_target_task_policy
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

# Operator-selected LAN peer and Wi-Fi policy enter through the allowlisted
# CMake cache of this firmware action only (`--define=H2_IPERF_SERVER=...`).
if(DEFINED H2_IPERF_SERVER)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_IPERF_SERVER="${H2_IPERF_SERVER}")
endif()
if(DEFINED H2_IPERF_POWER_SAVE)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_IPERF_POWER_SAVE=${H2_IPERF_POWER_SAVE})
endif()
if(DEFINED H2_IPERF_BLE_ADV)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_IPERF_BLE_ADV=${H2_IPERF_BLE_ADV})
endif()
if(DEFINED H2_IPERF_ROLE_SERVER)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_IPERF_ROLE_SERVER=${H2_IPERF_ROLE_SERVER})
endif()
