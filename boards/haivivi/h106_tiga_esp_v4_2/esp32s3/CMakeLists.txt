set(H2_ESP_BOARD_INCLUDE_DIRS
  "include"
  "$ENV{H2_GIZOS_ROOT}/libs/sync/atomic/include"
  "$ENV{H2_GIZOS_ROOT}/pal/include"
  "$ENV{H2_GIZOS_ROOT}/libs/runtime/include")

set(H2_ESP_BOARD_REQUIRES
  esp_driver_gpio
  esp_driver_ledc
  esp_driver_spi
  esp_driver_tsens
  esp_driver_uart
  esp_event
  esp_netif
  esp_adc
  esp_lcd
  esp_modem
  esp_driver_i2c
  h2_adc_oneshot
  h2_es8311_audio_system
  h2_nfc_fm175xx
  h2_firmware_lib
  led_strip
  h2_pal_core
  h2_firmware_lib
  joltwallet__littlefs
  h2_firmware_lib
  spi_flash)

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(H2_BOARD_SOURCES h2_esp_board)

idf_component_register(
  SRCS ${H2_BOARD_SOURCES}
  INCLUDE_DIRS
    ${H2_ESP_BOARD_INCLUDE_DIRS}
  REQUIRES
    ${H2_ESP_BOARD_REQUIRES})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# The modem RX tap observes the exact UART bytes esp_modem reads; see
# h2_esp_board_modem_quectel.c.
target_link_options(${COMPONENT_LIB} INTERFACE
  "-Wl,--wrap=uart_read_bytes"
  "-Wl,--wrap=uart_write_bytes"
  "-Wl,--wrap=xEventGroupWaitBits")
