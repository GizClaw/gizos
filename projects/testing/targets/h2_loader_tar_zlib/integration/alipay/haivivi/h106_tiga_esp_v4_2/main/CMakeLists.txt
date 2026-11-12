include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
include("$ENV{H2_BAZEL_COMPONENT_MANIFEST}")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)
set(H2_ALIPAY_REFERENCE_COMPONENTS)
if("h2_alipay" IN_LIST H2_BAZEL_COMPONENT_NAMES)
  list(APPEND H2_ALIPAY_REFERENCE_COMPONENTS h2_alipay)
endif()
idf_component_register(
  SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS "."
  REQUIRES
    h2_esp_target_task_policy
    ${H2_ALIPAY_REFERENCE_COMPONENTS}
    h2_firmware_lib
    h2_esp_board
    h2_pal_core
    lvgl_port
    esp_netif
    lwip
    app_update
    espcoredump
    esp_driver_uart
    esp_driver_usb_serial_jtag)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
include("$ENV{H2_GIZOS_ROOT}/projects/testing/libs/fixture-wifi/fixture_wifi.cmake")
