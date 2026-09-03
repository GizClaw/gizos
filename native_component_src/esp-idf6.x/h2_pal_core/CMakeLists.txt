set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

option(H2_ESP_PLATFORM_ENABLE_WEBRTC "Enable ESP WebRTC platform implementation backed by H2Peer" OFF)

set(H2_ESP_PLATFORM_CORE_SRCS
  "src/h2_esp_lazy_audio.c"
  "src/h2_esp_platform_mem.c"
  "src/h2_esp_platform_log.c"
  "src/h2_esp_platform_firmware_info.c"
  "src/h2_esp_platform_partition.c"
  "src/h2_esp_platform_sync.c"
  "src/h2_esp_platform_net.c"
  "src/h2_esp_platform_netif.c"
  "src/h2_esp_platform_task.c"
  "src/h2_esp_platform_queue.c"
  "src/h2_esp_platform_safe_call.c"
  "src/h2_esp_platform_time.c"
  "src/h2_esp_platform_crypto.c"
  "src/h2_esp_platform_dtls.c"
  "src/h2_esp_platform_dtls_state.c"
  "src/h2_esp_platform_net_socket.c"
  "src/h2_esp_platform_power.c"
  "src/h2_esp_platform_disk.c"
  "src/h2_esp_platform_littlefs_io.c"
  "src/h2_esp_platform_pref_littlefs.c"
  "src/h2_esp_platform_pref_migration.c"
  "src/h2_esp_platform_pref_nvs_legacy.c"
  "src/h2_esp_platform_pref_store.c"
  "src/h2_esp_platform_system_event.c"
  "src/h2_esp_platform_modem_unsupported.c"
  "src/h2_esp_ble_exact_adapter.c"
  "src/h2_esp_platform_ble.c"
  "src/h2_esp_platform_littlefs.c"
  "src/h2_esp_platform_spiffs.c")

if(IDF_TARGET STREQUAL "esp32p4" AND NOT CONFIG_ESP_WIFI_REMOTE_ENABLED)
  list(APPEND H2_ESP_PLATFORM_CORE_SRCS
    "src/h2_esp_platform_wifi_unsupported.c"
    "src/h2_esp_platform_wifi_settings_unsupported.c"
    "src/h2_esp_platform_wifi_csi_unsupported.c")
else()
  list(APPEND H2_ESP_PLATFORM_CORE_SRCS
    "src/h2_esp_platform_wifi.c"
    "src/h2_esp_platform_wifi_settings.c"
    "src/h2_esp_wifi_teardown.c")
  if(CONFIG_ESP_WIFI_CSI_ENABLED)
    list(APPEND H2_ESP_PLATFORM_CORE_SRCS "src/h2_esp_platform_wifi_csi.c")
  else()
    list(APPEND H2_ESP_PLATFORM_CORE_SRCS "src/h2_esp_platform_wifi_csi_unsupported.c")
  endif()
endif()

set(H2_ESP_PLATFORM_CORE_REQUIRES
  bt
  esp_event
  esp_netif
  esp_timer
  esp_wifi
  app_update
  esp_driver_uart
  esp_driver_usb_serial_jtag
  freertos
  joltwallet__littlefs
  lwip
  mbedtls
  nvs_flash
  spiffs)

if(CONFIG_ESP_CONSOLE_UART)
  list(APPEND H2_ESP_PLATFORM_CORE_SRCS
    "src/h2_esp_platform_uart_io_stream.c")
endif()

if(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
  list(APPEND H2_ESP_PLATFORM_CORE_SRCS
    "src/h2_esp_platform_usb_jtag_io_stream.c")
endif()

if(H2_ESP_PLATFORM_ENABLE_WEBRTC)
  list(APPEND H2_ESP_PLATFORM_CORE_SRCS "src/h2_esp_platform_webrtc.c")
else()
  list(APPEND H2_ESP_PLATFORM_CORE_SRCS "src/h2_esp_platform_webrtc_unsupported.c")
endif()

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_select_sources(
  H2_COMPONENT_SOURCES
  h2_pal_core
  ${H2_ESP_PLATFORM_CORE_SRCS})

idf_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
  INCLUDE_DIRS
    "include"
    "${REPO_ROOT}/libs/pal/include"
    "${REPO_ROOT}/libs/pal/providers/corehttp/include"
    "${REPO_ROOT}/libs/pal/providers/h2peer/include"
    "${REPO_ROOT}/libs/pal/providers/h2sctp/include"
    "${REPO_ROOT}/libs/pal/providers/libco/include"
  PRIV_REQUIRES
    ${H2_ESP_PLATFORM_CORE_REQUIRES}
    h2_firmware_lib
    h2_firmware_lib)

if(IDF_TARGET STREQUAL "esp32p4")
  idf_component_optional_requires(PRIVATE espressif__esp_hosted)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
