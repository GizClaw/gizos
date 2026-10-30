include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)

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
    h2_h2loader_runtime
    h2_pal_core)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
