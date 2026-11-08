if(NOT DEFINED H2_ESP_BOARD_IDENTITY)
  include("$ENV{H2_BAZEL_COMPONENT_MANIFEST}")
  set(H2_ESP_PHYSICAL_BOARD "h200")
  set(H2_ESP_BOARD_IDENTITY "${H2_BAZEL_BOARD}")
  if(H2_ESP_BOARD_IDENTITY STREQUAL "h200")
    set(H2_ESP_H200_BATTERY_PROFILE "original")
  elseif(H2_ESP_BOARD_IDENTITY STREQUAL "h200_v2")
    set(H2_ESP_H200_BATTERY_PROFILE "donghui")
  endif()
endif()

foreach(H2_H200_REQUIRED H2_ESP_PHYSICAL_BOARD H2_ESP_BOARD_IDENTITY H2_ESP_H200_BATTERY_PROFILE)
  if(NOT DEFINED ${H2_H200_REQUIRED} OR "${${H2_H200_REQUIRED}}" STREQUAL "")
    message(FATAL_ERROR "H200 profile is missing ${H2_H200_REQUIRED}")
  endif()
endforeach()
if(NOT H2_ESP_PHYSICAL_BOARD STREQUAL "h200")
  message(FATAL_ERROR "H200 physical board must be h200, got ${H2_ESP_PHYSICAL_BOARD}")
endif()
if(H2_ESP_BOARD_IDENTITY STREQUAL "h200")
  if(NOT H2_ESP_H200_BATTERY_PROFILE STREQUAL "original")
    message(FATAL_ERROR "h200 identity requires original battery profile")
  endif()
  set(H2_H200_BATTERY_COMPILE_DEFINITION H2_ESP_H200_BATTERY_PROFILE_ORIGINAL=1)
elseif(H2_ESP_BOARD_IDENTITY STREQUAL "h200_v2")
  if(NOT H2_ESP_H200_BATTERY_PROFILE STREQUAL "donghui")
    message(FATAL_ERROR "h200_v2 identity requires donghui battery profile")
  endif()
  set(H2_H200_BATTERY_COMPILE_DEFINITION H2_ESP_H200_BATTERY_PROFILE_DONGHUI=1)
else()
  message(FATAL_ERROR "unknown H200 identity: ${H2_ESP_BOARD_IDENTITY}")
endif()

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(H2_BOARD_SOURCES h2_esp_board)

idf_component_register(
  SRCS ${H2_BOARD_SOURCES}
  INCLUDE_DIRS
    "include"
    "$ENV{H2_GIZOS_ROOT}/pal/include"
    "$ENV{H2_GIZOS_ROOT}/libs/runtime/include"
  REQUIRES
    esp_adc
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_ledc
    esp_driver_spi
    h2_adc_oneshot
    h2_es8311_es7210_audio_system
    h2_nfc_fm175xx
    h2_pal_core
    h2_firmware_lib
    joltwallet__littlefs
    led_strip)

target_compile_definitions(
  ${COMPONENT_LIB}
  PRIVATE
    H2_ESP_BOARD_IDENTITY="${H2_ESP_BOARD_IDENTITY}"
    ${H2_H200_BATTERY_COMPILE_DEFINITION})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
