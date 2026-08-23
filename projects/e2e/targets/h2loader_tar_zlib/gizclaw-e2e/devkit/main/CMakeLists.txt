if(NOT DEFINED H2_REPO_ROOT)
  get_filename_component(H2_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../.." ABSOLUTE)
else()
  get_filename_component(H2_REPO_ROOT "${H2_REPO_ROOT}" ABSOLUTE)
endif()

set(H2_GIZCLAW_E2E_APP_DIR "${H2_REPO_ROOT}/projects/e2e/apps/gizclaw/app")
set(H2_GIZCLAW_E2E_VOICE_PROMPT
  "${H2_REPO_ROOT}/projects/e2e/apps/gizclaw/data/voice_prompt_l16_16khz_mono.pcm")

include("$ENV{H2_REPO_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)

idf_component_register(
  SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
    "${H2_GIZCLAW_E2E_APP_DIR}/include"
    "${H2_GIZCLAW_E2E_APP_DIR}/src"
    "${H2_REPO_ROOT}/libs/gizclaw/include"
    "${H2_REPO_ROOT}/libs/pal/include"
    "${H2_REPO_ROOT}/libs/runtime/include"
    "${H2_REPO_ROOT}/projects/h2loader/libs/h2loader/include"
  REQUIRES
    app_update
    espcoredump
    esp_netif
    esp_driver_uart
    esp_driver_usb_serial_jtag
    h2_firmware_lib
    h2_esp_board
    h2_gizclaw
    h2_h2loader_ble
    h2_h2loader_runtime
    h2_pal_core
    h2_firmware_lib
    opus_port)

target_add_binary_data(
  ${COMPONENT_LIB}
  "${H2_GIZCLAW_E2E_VOICE_PROMPT}"
  BINARY
  RENAME_TO h2_gizclaw_e2e_voice_prompt)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
