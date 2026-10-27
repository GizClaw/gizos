include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)
set(H2_SUITE_REQUIRES mbedtls esp_netif lwip)
list(APPEND H2_SUITE_REQUIRES esp_http_client)
if("h2_esp_audio_decoder" IN_LIST H2_BAZEL_COMPONENT_NAMES)
  list(APPEND H2_SUITE_REQUIRES h2_esp_audio_decoder)
endif()
idf_component_register(SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS "."
  REQUIRES h2_esp_target_task_policy h2_firmware_lib h2_esp_board h2_mosaico_loader_usb
    h2_h2loader_runtime h2_h2loader_ble h2_pal_core esp_timer app_update espcoredump
    ${H2_SUITE_REQUIRES})
target_compile_definitions(${COMPONENT_LIB} PRIVATE H2_E2E_MOSAICO=1)
# The Audio Decoder launcher uses the native bridge's public header. Match the
# maintained DevKit target's explicit include path for this prebuilt launcher.
target_include_directories(${COMPONENT_LIB} PRIVATE
  "${H2_REPO_ROOT}/native_component_src/esp-idf6.x/h2_esp_audio_decoder/include")
include("${H2_REPO_ROOT}/projects/e2e/targets/h2loader_tar_zlib/fixture-wifi/fixture_wifi.cmake")
# Reuse the public fixture inputs for the Mosaico performance entry; its peer
# must implement the performance wire protocol, not only PAL echo channels.
foreach(H2_WEBRTC_ENDPOINT H2_AEC_SOURCE_URL H2_AEC_SOURCE_TOKEN H2_PAL_WEBRTC_OFFER_URL H2_PAL_WEBRTC_STUN_URL H2_MOSAICO_IPERF_HOST H2_MOSAICO_PERF_BSSID)
  if(DEFINED ${H2_WEBRTC_ENDPOINT})
    if("${${H2_WEBRTC_ENDPOINT}}" MATCHES "[\r\n;]")
      message(FATAL_ERROR "${H2_WEBRTC_ENDPOINT} contains an unsupported character")
    endif()
    string(REPLACE "\\" "\\\\" H2_WEBRTC_ESCAPED "${${H2_WEBRTC_ENDPOINT}}")
    string(REPLACE "\"" "\\\"" H2_WEBRTC_ESCAPED "${H2_WEBRTC_ESCAPED}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
      "${H2_WEBRTC_ENDPOINT}=\"${H2_WEBRTC_ESCAPED}\"")
  endif()
endforeach()
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)

target_include_directories(${COMPONENT_LIB} PRIVATE
  "${H2_REPO_ROOT}/projects/e2e/targets/h2loader_tar_zlib/iperf-server/amoled/main")
