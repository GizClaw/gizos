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
    "${H2_REPO_ROOT}/projects/e2e/apps/webrtc-performance/app/include"
    "${H2_REPO_ROOT}/libs/pal/include"
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
    h2_pal_core
    opus_port)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)

set(H2_WEBRTC_PERF_WIFI_SSID "$ENV{H2LOADER_WIFI_SSID}")
if(H2_WEBRTC_PERF_WIFI_SSID STREQUAL "")
  set(H2_WEBRTC_PERF_WIFI_SSID "HAIVIVI-MFG")
endif()
set(H2_WEBRTC_PERF_WIFI_PASSWORD "$ENV{H2LOADER_WIFI_PASSWORD}")
if(H2_WEBRTC_PERF_WIFI_PASSWORD STREQUAL "")
  message(FATAL_ERROR
          "H2LOADER_WIFI_PASSWORD is required for the WebRTC performance image")
endif()
foreach(H2_WEBRTC_PERF_WIFI_VALUE
        H2_WEBRTC_PERF_WIFI_SSID
        H2_WEBRTC_PERF_WIFI_PASSWORD)
  if("${${H2_WEBRTC_PERF_WIFI_VALUE}}" MATCHES "[\r\n;]")
    message(FATAL_ERROR "${H2_WEBRTC_PERF_WIFI_VALUE} contains an unsupported character")
  endif()
  string(REPLACE "\\" "\\\\" ${H2_WEBRTC_PERF_WIFI_VALUE}
                 "${${H2_WEBRTC_PERF_WIFI_VALUE}}")
  string(REPLACE "\"" "\\\"" ${H2_WEBRTC_PERF_WIFI_VALUE}
                 "${${H2_WEBRTC_PERF_WIFI_VALUE}}")
endforeach()
target_compile_definitions(
  ${COMPONENT_LIB}
  PRIVATE
    H2_WEBRTC_PERF_WIFI_SSID="${H2_WEBRTC_PERF_WIFI_SSID}"
    H2_WEBRTC_PERF_WIFI_PASSWORD="${H2_WEBRTC_PERF_WIFI_PASSWORD}")

if(DEFINED H2_WEBRTC_PERF_ENDPOINT)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_WEBRTC_PERF_ENDPOINT="${H2_WEBRTC_PERF_ENDPOINT}")
endif()
if(DEFINED H2_WEBRTC_PERF_STUN_URL)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_WEBRTC_PERF_STUN_URL="${H2_WEBRTC_PERF_STUN_URL}")
endif()
