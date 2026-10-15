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
    h2_mosaico_loader_usb
    h2_h2loader_ble
    h2_h2loader_runtime
    h2_pal_core
    opus_port)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)

# Operator-selected LAN endpoints and Wi-Fi policy enter through the
# allowlisted CMake cache of this firmware action only. Wi-Fi credentials are
# not baked into the image; the Loader's `wifi connect` stores them.
if(DEFINED H2_WEBRTC_PERF_ENDPOINT)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_WEBRTC_PERF_ENDPOINT="${H2_WEBRTC_PERF_ENDPOINT}")
endif()
if(DEFINED H2_WEBRTC_PERF_STUN_URL)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_WEBRTC_PERF_STUN_URL="${H2_WEBRTC_PERF_STUN_URL}")
endif()
if(DEFINED H2_WEBRTC_PERF_POWER_SAVE)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_WEBRTC_PERF_POWER_SAVE=${H2_WEBRTC_PERF_POWER_SAVE})
endif()
if(DEFINED H2_WEBRTC_PERF_BLE_ADV)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_WEBRTC_PERF_BLE_ADV=${H2_WEBRTC_PERF_BLE_ADV})
endif()

if(DEFINED H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS)
  if(NOT H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS MATCHES "^[0-9]+$" OR H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS GREATER 60000)
    message(FATAL_ERROR "network preflight must be an integer from 0 through 60000 ms")
  endif()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS=${H2_WEBRTC_PERF_NETWORK_PREFLIGHT_MS})
endif()

# Diagnostic wrappers preserve every SDK operation and queue capacity.
if(DEFINED H2_WEBRTC_PERF_MBOX_DIAGNOSTICS)
  if(NOT H2_WEBRTC_PERF_MBOX_DIAGNOSTICS MATCHES "^[01]$")
    message(FATAL_ERROR "mailbox diagnostics must be 0 or 1")
  endif()
  if(H2_WEBRTC_PERF_MBOX_DIAGNOSTICS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE H2_WEBRTC_PERF_MBOX_DIAGNOSTICS=1)
    target_link_options(${COMPONENT_LIB} INTERFACE
      "-Wl,--wrap=sys_mbox_new" "-Wl,--wrap=sys_mbox_trypost" "-Wl,--wrap=sys_mbox_free")
  endif()
endif()

# Optional AP selection belongs to the private fixture, never saved credentials.
if(DEFINED H2_WEBRTC_PERF_WIFI_BSSID_HEX)
  string(LENGTH "${H2_WEBRTC_PERF_WIFI_BSSID_HEX}" bssid_length)
  if(NOT bssid_length EQUAL 12 OR NOT H2_WEBRTC_PERF_WIFI_BSSID_HEX MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "Wi-Fi BSSID must contain exactly 12 hexadecimal digits")
  endif()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    H2_WEBRTC_PERF_WIFI_BSSID_HEX="${H2_WEBRTC_PERF_WIFI_BSSID_HEX}")
endif()
