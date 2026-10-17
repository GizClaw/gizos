include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)

set(H2_E2E_REQUIRES)
if(H2_E2E_SUITE STREQUAL "pal-mqtt")
  list(APPEND H2_E2E_REQUIRES mbedtls)
elseif(H2_E2E_SUITE STREQUAL "pal-audio-decoder")
  list(APPEND H2_E2E_REQUIRES h2_esp_audio_decoder)
elseif(H2_E2E_SUITE STREQUAL "gizclaw-e2e")
  list(APPEND H2_E2E_REQUIRES h2_gizclaw esp_netif opus_port)
elseif(H2_E2E_SUITE STREQUAL "pal-ipv6-fixture")
  list(APPEND H2_E2E_REQUIRES esp_netif lwip mbedtls esp_timer esp_hw_support)
elseif(H2_E2E_SUITE STREQUAL "pal-ipv6")
  list(APPEND H2_E2E_REQUIRES esp_netif)
elseif(H2_E2E_SUITE STREQUAL "webrtc-performance")
  list(APPEND H2_E2E_REQUIRES opus_port)
endif()

idf_component_register(
  SRCS ${H2_MAIN_SOURCES}
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
    ${H2_E2E_REQUIRES})

target_compile_options(${COMPONENT_LIB} PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11> -Wall -Wextra -Werror)

if(H2_E2E_SUITE STREQUAL "gizclaw-e2e")
  target_add_binary_data(${COMPONENT_LIB}
    "$ENV{H2_GIZOS_ROOT}/projects/e2e/apps/gizclaw/data/voice_prompt_l16_16khz_mono.pcm"
    BINARY RENAME_TO h2_gizclaw_e2e_voice_prompt)
endif()

foreach(string_setting H2_IPERF_SERVER H2_WEBRTC_PERF_ENDPOINT
        H2_WEBRTC_PERF_STUN_URL H2_WEBRTC_PERF_WIFI_SSID
        H2_WEBRTC_PERF_WIFI_PASSWORD)
  if(DEFINED ${string_setting})
    if("${${string_setting}}" MATCHES "[\r\n;]")
      message(FATAL_ERROR "${string_setting} contains an unsupported character")
    endif()
    string(REPLACE "\\" "\\\\" ${string_setting} "${${string_setting}}")
    string(REPLACE "\"" "\\\"" ${string_setting} "${${string_setting}}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
      ${string_setting}="${${string_setting}}")
  endif()
endforeach()
foreach(number_setting H2_IPERF_POWER_SAVE H2_IPERF_BLE_ADV H2_IPERF_ROLE_SERVER)
  if(DEFINED ${number_setting})
    if(NOT "${${number_setting}}" MATCHES "^[0-2]$")
      message(FATAL_ERROR "${number_setting} must be a supported integer")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
      ${number_setting}=${${number_setting}})
  endif()
endforeach()
